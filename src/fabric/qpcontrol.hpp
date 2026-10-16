#pragma once

#include "fabric/qpsettings.hpp"

#include <rdma/rdma_cma.h>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

// The ACK timeout and retry count of the verbs provider's queue pairs.
//
// MXL 218ddaa creates its endpoints inside its fabrics library, and libfabric 2.3.1's verbs provider has no
// setting for either: it connects through rdma_cm with retry_count 15 (the kernel takes 7) and never calls
// rdma_set_option, so a RoCE queue pair gets rdma_cm's timeout 17 (537 ms). The agent binary defines
// rdma_connect, rdma_accept, rdma_get_cm_event and rdma_destroy_id itself (src/fabric/rdmahook.cpp); the dynamic
// linker binds libfabric's calls to them, and they call librdmacm's (dlsym RTLD_NEXT). Before a connect or an
// accept the hook sets RDMA_OPTION_ID_ACK_TIMEOUT on the rdma_cm id (the kernel applies it when it moves the
// queue pair to RTS) and the connect's retry_count (the accepting side's queue pair takes the initiator's). When the
// connection is established it reads both back from the queue pair (ibv_query_qp) and logs them with the lane's
// flow and peer (fabric_qp). LD_PRELOAD would not do: the image's binary has file capabilities, and the loader
// ignores LD_PRELOAD for it.
//
// Whether the NIC uses the timeout is the driver's business: mlx5 and rxe do; Linux's in-tree irdma (E810)
// ignores it (its retransmit timer is the module's roce_rtomin), and its ibv_query_qp reports no timeout.
namespace mfa::qp
{
// What the calling lane does, so the hook can name the flow and peer of a connection. libfabric's verbs provider
// calls rdma_cm on the thread that called into MXL (connect and accept from MakeProgress and the target's reads).
struct Context
{
    char const* role = "";               // "source" (initiator) or "destination" (target)
    std::string const* flowId = nullptr;
    std::string const* peer = nullptr;   // destination: the source host
    // source: the destination host of a remote address "ip:port" (empty when unknown)
    std::function<std::string(std::string const&)> const* peerOf = nullptr;
};

// Sets the thread's context for its lifetime (nests).
class Scope
{
public:
    explicit Scope(Context const& context);
    ~Scope();
    Scope(Scope const&) = delete;
    Scope& operator=(Scope const&) = delete;

private:
    Context const* previous_;
};

Context const* current();

// "ip:port" of an IPv4 or IPv6 socket address; "" for anything else.
std::string addressText(sockaddr const* addr);
// "ip:port" of an MXL target info's fabricAddress (base64 of the target's socket address); "" when it is none.
std::string targetAddress(std::string const& targetInfoJson);

// One established queue pair.
struct Report
{
    std::string role;
    std::string flowId;
    std::string peer;
    std::string local;
    std::string remote;
    std::string device;
    std::uint32_t qpNum = 0;
    int wantAckTimeout = 0;   // the setting (0: left to rdma_cm)
    int wantRetryCount = 0;   // the setting; a destination's queue pair takes the source's
    bool optionSet = false;   // rdma_set_option took the timeout
    std::string optionError;  // why not
    bool queried = false;     // ibv_query_qp answered
    std::string queryError;
    int ackTimeout = -1;      // read back from the queue pair (0: the driver reports none)
    int retryCount = -1;
    int rnrRetry = -1;
};

// librdmacm and libibverbs entry points (the real ones, or a test's).
struct Ops
{
    int (*setOption)(rdma_cm_id*, int, int, void*, size_t) = nullptr;
    int (*connect)(rdma_cm_id*, rdma_conn_param*) = nullptr;
    int (*accept)(rdma_cm_id*, rdma_conn_param*) = nullptr;
    int (*getEvent)(rdma_event_channel*, rdma_cm_event**) = nullptr;
    int (*destroyId)(rdma_cm_id*) = nullptr;
    int (*queryQp)(ibv_qp*, ibv_qp_attr*, int, ibv_qp_init_attr*) = nullptr;
};

class Hooks
{
public:
    using Sink = std::function<void(Report const&)>;

    explicit Hooks(Ops ops, Sink sink = {});

    int connect(rdma_cm_id* id, rdma_conn_param* param);
    int accept(rdma_cm_id* id, rdma_conn_param* param);
    int getEvent(rdma_event_channel* channel, rdma_cm_event** event);
    int destroyId(rdma_cm_id* id);

    // Connections set up and not yet established or gone.
    std::size_t pending() const;

private:
    void prepare(rdma_cm_id* id, char const* role, Settings const& settings);
    void established(rdma_cm_id* id);
    void forget(rdma_cm_id* id);

    Ops ops_;
    Sink sink_;
    mutable std::mutex mu_;
    std::map<rdma_cm_id*, Report> pending_;
};

// The agent's sink: logs fabric_qp and counts it for fabric_qp_established_total.
void record(Report const& report);

// fabric_qp_established_total: by role, the read-back ack_timeout ("unreported" when the driver reports none) and
// retry count.
using EstablishedKey = std::tuple<std::string, std::string, std::string>;
std::map<EstablishedKey, std::uint64_t> establishedCounts();
// fabric_qp_option_failures_total: rdma_set_option refused the timeout.
std::uint64_t optionFailures();

// The binary's rdma_cm hooks are the ones the dynamic linker hands out, and librdmacm's are behind them
// (src/fabric/rdmahook.cpp; only in binaries that link it).
bool hooksResolved();
} // namespace mfa::qp
