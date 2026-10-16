#include <doctest/doctest.h>

#include "fabric/qpcontrol.hpp"

#include <arpa/inet.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

using namespace mfa;

namespace
{
// A fake librdmacm and libibverbs: records the calls in order.
struct Fake
{
    std::vector<std::string> calls;
    int optionLevel = -1;
    int optionName = -1;
    std::size_t optionLen = 0;
    int optionValue = -1;
    int optionResult = 0;
    int connectRetry = -1;
    int connectResult = 0;
    rdma_cm_event* event = nullptr;
    ibv_qp_attr attr{};
    int queryResult = 0;
};
Fake* gFake = nullptr;

int fakeSetOption(rdma_cm_id*, int level, int name, void* value, size_t len)
{
    gFake->calls.push_back("set_option");
    gFake->optionLevel = level;
    gFake->optionName = name;
    gFake->optionLen = len;
    gFake->optionValue = len == 1 ? *static_cast<std::uint8_t*>(value) : -1;
    // As the kernel (ucma_set_option_level): the ACK timeout is an RDMA_OPTION_ID option of one byte; the IB level
    // only knows RDMA_OPTION_IB_PATH and answers ENOSYS.
    if (level != RDMA_OPTION_ID)
    {
        errno = ENOSYS;
        return -1;
    }
    if (name != RDMA_OPTION_ID_ACK_TIMEOUT || len != 1)
    {
        errno = EINVAL;
        return -1;
    }
    if (gFake->optionResult != 0)
    {
        errno = gFake->optionResult;
        return -1;
    }
    return 0;
}

int fakeConnect(rdma_cm_id*, rdma_conn_param* param)
{
    gFake->calls.push_back("connect");
    gFake->connectRetry = param != nullptr ? param->retry_count : -1;
    if (gFake->connectResult != 0)
    {
        errno = gFake->connectResult;
        return -1;
    }
    return 0;
}

int fakeAccept(rdma_cm_id*, rdma_conn_param*)
{
    gFake->calls.push_back("accept");
    return 0;
}

int fakeGetEvent(rdma_event_channel*, rdma_cm_event** event)
{
    gFake->calls.push_back("get_event");
    *event = gFake->event;
    return 0;
}

int fakeDestroy(rdma_cm_id*)
{
    gFake->calls.push_back("destroy");
    return 0;
}

void fakeDestroyEp(rdma_cm_id*)
{
    gFake->calls.push_back("destroy_ep");
}

int fakeQuery(ibv_qp*, ibv_qp_attr* attr, int mask, ibv_qp_init_attr*)
{
    gFake->calls.push_back("query");
    CHECK((mask & IBV_QP_TIMEOUT) != 0);
    CHECK((mask & IBV_QP_RETRY_CNT) != 0);
    *attr = gFake->attr;
    return gFake->queryResult;
}

qp::Ops fakeOps()
{
    qp::Ops ops;
    ops.setOption = fakeSetOption;
    ops.connect = fakeConnect;
    ops.accept = fakeAccept;
    ops.getEvent = fakeGetEvent;
    ops.destroyId = fakeDestroy;
    ops.destroyEp = fakeDestroyEp;
    ops.queryQp = fakeQuery;
    return ops;
}

void setAddress(sockaddr* out, char const* ip, int port)
{
    auto* in = reinterpret_cast<sockaddr_in*>(out);
    in->sin_family = AF_INET;
    in->sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, ip, &in->sin_addr);
}

std::string base64(unsigned char const* data, std::size_t len)
{
    static char const* const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < len; i += 3)
    {
        unsigned int chunk = static_cast<unsigned int>(data[i]) << 16;
        if (i + 1 < len)
        {
            chunk |= static_cast<unsigned int>(data[i + 1]) << 8;
        }
        if (i + 2 < len)
        {
            chunk |= data[i + 2];
        }
        out += alphabet[(chunk >> 18) & 63];
        out += alphabet[(chunk >> 12) & 63];
        out += i + 1 < len ? alphabet[(chunk >> 6) & 63] : '=';
        out += i + 2 < len ? alphabet[chunk & 63] : '=';
    }
    return out;
}

struct Connection
{
    ibv_device device{};
    ibv_context context{};
    ibv_qp queuePair{};
    rdma_cm_id id{};
    rdma_cm_event event{};

    Connection()
    {
        std::strcpy(device.name, "mlx5_0");
        context.device = &device;
        queuePair.qp_num = 0x1234;
        id.qp_type = IBV_QPT_RC;
        id.verbs = &context;
        id.qp = &queuePair;
        setAddress(&id.route.addr.src_addr, "192.168.250.1", 40000);
        setAddress(&id.route.addr.dst_addr, "192.168.250.2", 23501);
        event.id = &id;
        event.event = RDMA_CM_EVENT_ESTABLISHED;
    }
};
} // namespace

TEST_CASE("connect sets the ACK timeout before rdma_connect and the retry count in its parameters")
{
    Fake fake;
    gFake = &fake;
    fake.attr.timeout = 14;
    fake.attr.retry_cnt = 7;
    fake.attr.rnr_retry = 7;
    std::vector<qp::Report> reports;
    qp::Hooks hooks(fakeOps(), [&](qp::Report const& r) { reports.push_back(r); });
    qp::configure({14, 7});

    Connection c;
    rdma_conn_param param{};
    param.retry_count = 15; // libfabric 2.3.1
    param.rnr_retry_count = 7;
    std::string const flow = "f-audio";
    std::function<std::string(std::string const&)> const peerOf = [](std::string const& remote) {
        return remote == "192.168.250.2:23501" ? std::string("host-02") : std::string();
    };
    qp::Context context;
    context.role = "source";
    context.flowId = &flow;
    context.peerOf = &peerOf;
    {
        qp::Scope const scope{context};
        CHECK(hooks.connect(&c.id, &param) == 0);
    }
    CHECK(fake.calls == std::vector<std::string>{"set_option", "connect"});
    CHECK(fake.optionLevel == RDMA_OPTION_ID);
    CHECK(fake.optionName == RDMA_OPTION_ID_ACK_TIMEOUT);
    CHECK(fake.optionLen == 1);
    CHECK(fake.optionValue == 14);
    CHECK(fake.connectRetry == 7);
    CHECK(param.retry_count == 15); // libfabric's own copy is untouched
    CHECK(hooks.pending() == 1);

    // Established (on any thread): read back and reported with the flow and peer of the connect.
    rdma_cm_event* got = nullptr;
    fake.event = &c.event;
    CHECK(hooks.getEvent(nullptr, &got) == 0);
    CHECK(got == &c.event);
    REQUIRE(reports.size() == 1);
    auto const& r = reports[0];
    CHECK(r.role == "source");
    CHECK(r.flowId == "f-audio");
    CHECK(r.peer == "host-02");
    CHECK(r.remote == "192.168.250.2:23501");
    CHECK(r.local == "192.168.250.1:40000");
    CHECK(r.device == "mlx5_0");
    CHECK(r.qpNum == 0x1234);
    CHECK(r.optionSet);
    CHECK(r.optionError.empty());
    CHECK(r.queried);
    CHECK(r.ackTimeout == 14);
    CHECK(r.retryCount == 7);
    CHECK(r.wantAckTimeout == 14);
    CHECK(hooks.pending() == 0);
    // A second ESTABLISHED for the same id is not reported again.
    CHECK(hooks.getEvent(nullptr, &got) == 0);
    CHECK(reports.size() == 1);
}

TEST_CASE("accept sets the ACK timeout before rdma_accept; a driver that reports none is visible")
{
    Fake fake;
    gFake = &fake;
    fake.attr.timeout = 0; // irdma: ibv_query_qp has no timeout
    fake.attr.retry_cnt = 7;
    std::vector<qp::Report> reports;
    qp::Hooks hooks(fakeOps(), [&](qp::Report const& r) { reports.push_back(r); });
    qp::configure({12, 5});
    Connection c;
    rdma_conn_param param{};
    std::string const flow = "f-video";
    std::string const peer = "host-01";
    qp::Context context;
    context.role = "destination";
    context.flowId = &flow;
    context.peer = &peer;
    {
        qp::Scope const scope{context};
        CHECK(hooks.accept(&c.id, &param) == 0);
    }
    CHECK(fake.calls == std::vector<std::string>{"set_option", "accept"});
    CHECK(fake.optionValue == 12);
    fake.event = &c.event;
    rdma_cm_event* got = nullptr;
    hooks.getEvent(nullptr, &got);
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].role == "destination");
    CHECK(reports[0].peer == "host-01");
    CHECK(reports[0].flowId == "f-video");
    CHECK(reports[0].ackTimeout == 0);
    CHECK(reports[0].wantRetryCount == 5);
    qp::configure({});
}

TEST_CASE("FABRIC_ACK_TIMEOUT=0 leaves the timeout to rdma_cm; a refused option still connects")
{
    Fake fake;
    gFake = &fake;
    std::vector<qp::Report> reports;
    qp::Hooks hooks(fakeOps(), [&](qp::Report const& r) { reports.push_back(r); });
    Connection c;
    rdma_conn_param param{};
    qp::configure({0, 7});
    CHECK(hooks.connect(&c.id, &param) == 0);
    CHECK(fake.calls == std::vector<std::string>{"connect"});
    CHECK(fake.connectRetry == 7);

    fake.calls.clear();
    fake.optionResult = EINVAL;
    qp::configure({14, 7});
    Connection d;
    CHECK(hooks.connect(&d.id, &param) == 0);
    CHECK(fake.calls == std::vector<std::string>{"set_option", "connect"});
    fake.event = &d.event;
    rdma_cm_event* got = nullptr;
    hooks.getEvent(nullptr, &got);
    REQUIRE(reports.size() == 1);
    CHECK_FALSE(reports[0].optionSet);
    CHECK(reports[0].optionError == std::strerror(EINVAL));
    qp::configure({});
}

TEST_CASE("a failed connect, an error event or a destroyed id forget the connection")
{
    Fake fake;
    gFake = &fake;
    std::vector<qp::Report> reports;
    qp::Hooks hooks(fakeOps(), [&](qp::Report const& r) { reports.push_back(r); });
    rdma_conn_param param{};
    Connection a;
    fake.connectResult = ECONNREFUSED;
    errno = 0;
    CHECK(hooks.connect(&a.id, &param) == -1);
    CHECK(errno == ECONNREFUSED);
    CHECK(hooks.pending() == 0);
    fake.connectResult = 0;

    Connection b;
    CHECK(hooks.connect(&b.id, &param) == 0);
    b.event.event = RDMA_CM_EVENT_REJECTED;
    fake.event = &b.event;
    rdma_cm_event* got = nullptr;
    hooks.getEvent(nullptr, &got);
    CHECK(hooks.pending() == 0);

    Connection c;
    CHECK(hooks.accept(&c.id, &param) == 0);
    CHECK(hooks.destroyId(&c.id) == 0);
    CHECK(hooks.pending() == 0);

    // libfabric closes a connected endpoint with rdma_destroy_ep (librdmacm destroys the id inside).
    Connection e;
    CHECK(hooks.connect(&e.id, &param) == 0);
    CHECK(hooks.pending() == 1);
    fake.calls.clear();
    hooks.destroyEp(&e.id);
    CHECK(fake.calls == std::vector<std::string>{"destroy_ep"});
    CHECK(hooks.pending() == 0);
    CHECK(reports.empty());

    // A UD id (no ACK timeout) is not given the option.
    Connection d;
    d.id.qp_type = IBV_QPT_UD;
    fake.calls.clear();
    CHECK(hooks.connect(&d.id, &param) == 0);
    CHECK(fake.calls == std::vector<std::string>{"connect"});
}

TEST_CASE("without librdmacm the hooks fail with ENOSYS")
{
    qp::Hooks hooks(qp::Ops{});
    rdma_conn_param param{};
    Connection c;
    errno = 0;
    CHECK(hooks.connect(&c.id, &param) == -1);
    CHECK(errno == ENOSYS);
}

TEST_CASE("the address of an MXL target info")
{
    sockaddr_in in{};
    setAddress(reinterpret_cast<sockaddr*>(&in), "192.168.250.2", 23501);
    auto const encoded = base64(reinterpret_cast<unsigned char const*>(&in), sizeof(in));
    auto const info = std::string(R"({"fabricAddress":")") + encoded + R"(","addressFormat":2,"regions":[],"id":"1","provider":"verbs"})";
    CHECK(qp::targetAddress(info) == "192.168.250.2:23501");
    CHECK(qp::targetAddress(R"({"fabricAddress":"AAA"})").empty());
    CHECK(qp::targetAddress("not json").empty());
}

TEST_CASE("fabric_qp warns only when a reported value differs")
{
    qp::Report r;
    r.role = "source";
    r.wantAckTimeout = 14;
    r.wantRetryCount = 7;
    r.optionSet = true;
    r.queried = true;
    r.ackTimeout = 14;
    r.retryCount = 7;
    auto v = qp::judge(r);
    CHECK(v.checked);
    CHECK(v.applied);
    CHECK_FALSE(v.warn);
    // irdma: no timeout reported. Not checked, info.
    r.ackTimeout = 0;
    v = qp::judge(r);
    CHECK_FALSE(v.checked);
    CHECK(v.applied);
    CHECK_FALSE(v.warn);
    // A destination reports the same at the other end.
    r.role = "destination";
    CHECK_FALSE(qp::judge(r).warn);
    // A reported timeout that differs.
    r.ackTimeout = 17;
    v = qp::judge(r);
    CHECK(v.checked);
    CHECK_FALSE(v.applied);
    CHECK(v.warn);
    // The source's retry count differs; a destination's is the source's (not compared).
    r.ackTimeout = 14;
    r.role = "source";
    r.retryCount = 6;
    CHECK(qp::judge(r).warn);
    r.role = "destination";
    CHECK_FALSE(qp::judge(r).warn);
    // rdma_set_option refused the timeout.
    r.retryCount = 7;
    r.ackTimeout = 0;
    r.optionSet = false;
    r.optionError = "Function not implemented";
    CHECK(qp::judge(r).warn);
    // Left to rdma_cm: nothing to compare.
    qp::Report cm;
    cm.role = "source";
    cm.wantAckTimeout = 0;
    cm.wantRetryCount = 7;
    cm.queried = true;
    cm.ackTimeout = 17;
    cm.retryCount = 7;
    v = qp::judge(cm);
    CHECK_FALSE(v.checked);
    CHECK_FALSE(v.warn);
}
