#include "fabric/qpcontrol.hpp"

#include "util/jsonutil.hpp"
#include "util/logging.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace mfa::qp
{
namespace
{
thread_local Context const* tCurrent = nullptr;

// Never destroyed: libfabric may still call rdma_cm (and so record) while static objects are torn down after main.
std::mutex& countsMu()
{
    static auto* mu = new std::mutex;
    return *mu;
}

std::map<EstablishedKey, std::uint64_t>& counts()
{
    static auto* value = new std::map<EstablishedKey, std::uint64_t>;
    return *value;
}

std::uint64_t& failures()
{
    static std::uint64_t value = 0;
    return value;
}

std::vector<unsigned char> base64Decode(std::string const& text)
{
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
        {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z')
        {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9')
        {
            return c - '0' + 52;
        }
        if (c == '+' || c == '-')
        {
            return 62;
        }
        if (c == '/' || c == '_')
        {
            return 63;
        }
        return -1;
    };
    std::vector<unsigned char> out;
    unsigned int buffer = 0;
    int bits = 0;
    for (char const c : text)
    {
        if (c == '=')
        {
            break;
        }
        auto const v = value(c);
        if (v < 0)
        {
            return {};
        }
        buffer = (buffer << 6) | static_cast<unsigned int>(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

std::string idAddress(rdma_cm_id const* id, bool local)
{
    if (id == nullptr)
    {
        return {};
    }
    return addressText(local ? &id->route.addr.src_addr : &id->route.addr.dst_addr);
}
} // namespace

Scope::Scope(Context const& context)
    : previous_(tCurrent)
{
    tCurrent = &context;
}

Scope::~Scope()
{
    tCurrent = previous_;
}

Context const* current()
{
    return tCurrent;
}

std::string addressText(sockaddr const* addr)
{
    if (addr == nullptr)
    {
        return {};
    }
    char text[INET6_ADDRSTRLEN] = {};
    if (addr->sa_family == AF_INET)
    {
        auto const* in = reinterpret_cast<sockaddr_in const*>(addr);
        if (::inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text)) == nullptr)
        {
            return {};
        }
        return std::string(text) + ":" + std::to_string(ntohs(in->sin_port));
    }
    if (addr->sa_family == AF_INET6)
    {
        auto const* in6 = reinterpret_cast<sockaddr_in6 const*>(addr);
        if (::inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof(text)) == nullptr)
        {
            return {};
        }
        return "[" + std::string(text) + "]:" + std::to_string(ntohs(in6->sin6_port));
    }
    return {};
}

std::string targetAddress(std::string const& targetInfoJson)
{
    auto const root = json::objectOrEmpty(json::parse(targetInfoJson));
    auto const encoded = json::asString(root, "fabricAddress");
    if (!encoded)
    {
        return {};
    }
    auto const raw = base64Decode(*encoded);
    if (raw.size() >= sizeof(sockaddr_in))
    {
        sockaddr_storage storage{};
        std::memcpy(&storage, raw.data(), std::min(raw.size(), sizeof(storage)));
        auto const* addr = reinterpret_cast<sockaddr const*>(&storage);
        if (addr->sa_family == AF_INET || (addr->sa_family == AF_INET6 && raw.size() >= sizeof(sockaddr_in6)))
        {
            return addressText(addr);
        }
    }
    return {};
}

Hooks::Hooks(Ops ops, Sink sink)
    : ops_(ops)
    , sink_(std::move(sink))
{}

void Hooks::prepare(rdma_cm_id* id, char const* role, Settings const& settings)
{
    Report report;
    report.role = role;
    report.wantAckTimeout = settings.ackTimeout;
    report.wantRetryCount = settings.retryCount;
    report.local = idAddress(id, true);
    report.remote = idAddress(id, false);
    if (auto const* context = current(); context != nullptr)
    {
        report.role = context->role[0] != '\0' ? context->role : role;
        report.flowId = context->flowId != nullptr ? *context->flowId : std::string{};
        if (context->peer != nullptr)
        {
            report.peer = *context->peer;
        }
        else if (context->peerOf != nullptr && *context->peerOf)
        {
            report.peer = (*context->peerOf)(report.remote);
        }
    }
    // Only a reliable connection has an ACK timeout (rdma_set_ack_timeout refuses anything else).
    if (settings.ackTimeout != 0 && id->qp_type == IBV_QPT_RC && ops_.setOption != nullptr)
    {
        // Level RDMA_OPTION_ID (the IB level only knows RDMA_OPTION_IB_PATH and answers ENOSYS), and exactly one
        // byte (ucma_set_option_id).
        std::uint8_t timeout = static_cast<std::uint8_t>(settings.ackTimeout);
        errno = 0;
        if (ops_.setOption(id, RDMA_OPTION_ID, RDMA_OPTION_ID_ACK_TIMEOUT, &timeout, sizeof(timeout)) == 0)
        {
            report.optionSet = true;
        }
        else
        {
            report.optionError = std::strerror(errno != 0 ? errno : EINVAL);
            std::lock_guard const lock{countsMu()};
            failures() += 1;
        }
    }
    std::lock_guard const lock{mu_};
    pending_[id] = std::move(report);
}

int Hooks::connect(rdma_cm_id* id, rdma_conn_param* param)
{
    if (ops_.connect == nullptr)
    {
        errno = ENOSYS;
        return -1;
    }
    if (id == nullptr)
    {
        return ops_.connect(id, param);
    }
    auto const settings = qp::settings();
    prepare(id, "source", settings);
    if (param == nullptr)
    {
        return ops_.connect(id, param);
    }
    // The initiator's retry count goes into the connection request: both queue pairs of the connection take it.
    // libfabric's own parameters stay as they are (it keeps them for a later connect).
    rdma_conn_param copy = *param;
    copy.retry_count = static_cast<std::uint8_t>(settings.retryCount);
    auto const result = ops_.connect(id, &copy);
    if (result != 0)
    {
        auto const err = errno;
        forget(id);
        errno = err;
    }
    return result;
}

int Hooks::accept(rdma_cm_id* id, rdma_conn_param* param)
{
    if (ops_.accept == nullptr)
    {
        errno = ENOSYS;
        return -1;
    }
    if (id != nullptr)
    {
        prepare(id, "destination", qp::settings());
    }
    auto const result = ops_.accept(id, param);
    if (result != 0 && id != nullptr)
    {
        auto const err = errno;
        forget(id);
        errno = err;
    }
    return result;
}

int Hooks::getEvent(rdma_event_channel* channel, rdma_cm_event** event)
{
    if (ops_.getEvent == nullptr)
    {
        errno = ENOSYS;
        return -1;
    }
    auto const result = ops_.getEvent(channel, event);
    if (result != 0 || event == nullptr || *event == nullptr || (*event)->id == nullptr)
    {
        return result;
    }
    auto const err = errno;
    switch ((*event)->event)
    {
    case RDMA_CM_EVENT_ESTABLISHED: established((*event)->id); break;
    case RDMA_CM_EVENT_ADDR_ERROR:
    case RDMA_CM_EVENT_ROUTE_ERROR:
    case RDMA_CM_EVENT_CONNECT_ERROR:
    case RDMA_CM_EVENT_UNREACHABLE:
    case RDMA_CM_EVENT_REJECTED:
    case RDMA_CM_EVENT_DISCONNECTED:
    case RDMA_CM_EVENT_DEVICE_REMOVAL:
    case RDMA_CM_EVENT_TIMEWAIT_EXIT: forget((*event)->id); break;
    default: break;
    }
    errno = err;
    return result;
}

void Hooks::destroyEp(rdma_cm_id* id)
{
    forget(id); // librdmacm destroys the id inside, without the rdma_destroy_id hook
    if (ops_.destroyEp != nullptr)
    {
        ops_.destroyEp(id);
    }
}

int Hooks::destroyId(rdma_cm_id* id)
{
    if (ops_.destroyId == nullptr)
    {
        errno = ENOSYS;
        return -1;
    }
    forget(id); // before the id's memory can be reused by another id
    return ops_.destroyId(id);
}

std::size_t Hooks::pending() const
{
    std::lock_guard const lock{mu_};
    return pending_.size();
}

void Hooks::forget(rdma_cm_id* id)
{
    std::lock_guard const lock{mu_};
    pending_.erase(id);
}

void Hooks::established(rdma_cm_id* id)
{
    Report report;
    {
        std::lock_guard const lock{mu_};
        auto const it = pending_.find(id);
        if (it == pending_.end())
        {
            return;
        }
        report = std::move(it->second);
        pending_.erase(it);
    }
    // Addresses are complete now (the accepting id's peer, the initiator's source port).
    if (auto local = idAddress(id, true); !local.empty())
    {
        report.local = std::move(local);
    }
    if (auto remote = idAddress(id, false); !remote.empty())
    {
        report.remote = std::move(remote);
    }
    if (id->verbs != nullptr && id->verbs->device != nullptr)
    {
        report.device = id->verbs->device->name;
    }
    if (id->qp == nullptr)
    {
        report.queryError = "no queue pair on the rdma_cm id";
    }
    else
    {
        report.qpNum = id->qp->qp_num;
        if (ops_.queryQp == nullptr)
        {
            report.queryError = "ibv_query_qp unavailable";
        }
        else
        {
            ibv_qp_attr attr{};
            ibv_qp_init_attr init{};
            auto const rc = ops_.queryQp(id->qp, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY, &init);
            if (rc == 0)
            {
                report.queried = true;
                report.ackTimeout = attr.timeout;
                report.retryCount = attr.retry_cnt;
                report.rnrRetry = attr.rnr_retry;
            }
            else
            {
                report.queryError = std::strerror(rc > 0 ? rc : (errno != 0 ? errno : EINVAL));
            }
        }
    }
    if (sink_)
    {
        sink_(report);
    }
}

Verdict judge(Report const& report)
{
    Verdict verdict;
    // The timeout counts only where the queue pair reports one: irdma's ibv_query_qp gives none (0).
    verdict.checked = report.wantAckTimeout != 0 && report.queried && report.ackTimeout > 0;
    bool const timeoutOk = !verdict.checked || report.ackTimeout == report.wantAckTimeout;
    // The source's retry count goes into the connection request; a destination's queue pair takes the source's.
    bool const retryOk = report.role != "source" || !report.queried || report.retryCount == report.wantRetryCount;
    verdict.applied = timeoutOk && retryOk && report.optionError.empty();
    verdict.warn = !verdict.applied;
    return verdict;
}

void record(Report const& report)
{
    auto const timeoutText = !report.queried ? std::string("unknown") : report.ackTimeout == 0 ? std::string("unreported") : std::to_string(report.ackTimeout);
    auto const retryText = report.queried ? std::to_string(report.retryCount) : std::string("unknown");
    {
        std::lock_guard const lock{countsMu()};
        counts()[EstablishedKey{report.role, timeoutText, retryText}] += 1;
    }
    auto ms = [](int exponent) {
        auto const value = ackTimeoutSeconds(exponent) * 1000.0;
        auto text = std::to_string(std::round(value * 1000.0) / 1000.0);
        text.erase(text.find_last_not_of('0') + 1);
        if (!text.empty() && text.back() == '.')
        {
            text.pop_back();
        }
        return text;
    };
    std::vector<log::Field> fields{{"role", report.role}, {"flow_id", report.flowId}, {"peer", report.peer}, {"local", report.local},
        {"remote", report.remote}, {"device", report.device}, {"qp_num", std::to_string(report.qpNum)},
        {"ack_timeout", timeoutText}, {"retry_count", retryText}};
    if (report.queried && report.ackTimeout > 0)
    {
        fields.emplace_back("ack_timeout_ms", ms(report.ackTimeout));
    }
    if (report.queried)
    {
        fields.emplace_back("rnr_retry", std::to_string(report.rnrRetry));
    }
    fields.emplace_back("set_ack_timeout", report.wantAckTimeout == 0 ? std::string("rdma_cm") : std::to_string(report.wantAckTimeout));
    fields.emplace_back("set_retry_count", std::to_string(report.wantRetryCount));
    if (!report.optionError.empty())
    {
        fields.emplace_back("option_error", report.optionError);
    }
    if (!report.queryError.empty())
    {
        fields.emplace_back("query_error", report.queryError);
    }
    auto const verdict = judge(report);
    fields.emplace_back("checked", verdict.checked ? "true" : "false");
    fields.emplace_back("applied", verdict.applied ? "true" : "false");
    if (!verdict.warn)
    {
        log::info("fabric_qp", fields);
    }
    else
    {
        log::warn("fabric_qp", fields);
    }
}

std::map<EstablishedKey, std::uint64_t> establishedCounts()
{
    std::lock_guard const lock{countsMu()};
    return counts();
}

std::uint64_t optionFailures()
{
    std::lock_guard const lock{countsMu()};
    return failures();
}
} // namespace mfa::qp
