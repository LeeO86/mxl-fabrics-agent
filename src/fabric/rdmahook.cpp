// librdmacm entry points of the agent binary (see fabric/qpcontrol.hpp). libfabric's verbs provider calls these
// through the dynamic linker, which finds the executable's definitions first; they call librdmacm's own.
// CMakeLists.txt exports them from the executable (--export-dynamic-symbol).

#include "fabric/qpcontrol.hpp"

#include <dlfcn.h>

#include <cerrno>

namespace
{
template <typename F>
F next(char const* name)
{
    return reinterpret_cast<F>(::dlsym(RTLD_NEXT, name));
}

mfa::qp::Hooks& hooks()
{
    static mfa::qp::Hooks instance = [] {
        mfa::qp::Ops ops;
        ops.setOption = next<decltype(ops.setOption)>("rdma_set_option");
        ops.connect = next<decltype(ops.connect)>("rdma_connect");
        ops.accept = next<decltype(ops.accept)>("rdma_accept");
        ops.getEvent = next<decltype(ops.getEvent)>("rdma_get_cm_event");
        ops.destroyId = next<decltype(ops.destroyId)>("rdma_destroy_id");
        // libibverbs is loaded with librdmacm (libfabric needs both).
        ops.queryQp = reinterpret_cast<decltype(ops.queryQp)>(::dlsym(RTLD_DEFAULT, "ibv_query_qp"));
        return mfa::qp::Hooks(ops, &mfa::qp::record);
    }();
    return instance;
}
} // namespace

extern "C" {
int rdma_connect(struct rdma_cm_id* id, struct rdma_conn_param* conn_param)
{
    return hooks().connect(id, conn_param);
}

int rdma_accept(struct rdma_cm_id* id, struct rdma_conn_param* conn_param)
{
    return hooks().accept(id, conn_param);
}

int rdma_get_cm_event(struct rdma_event_channel* channel, struct rdma_cm_event** event)
{
    return hooks().getEvent(channel, event);
}

int rdma_destroy_id(struct rdma_cm_id* id)
{
    return hooks().destroyId(id);
}
}

namespace mfa::qp
{
// The real librdmacm when the hooks are in place (the agent and lane-tests), for the startup log.
bool hooksResolved()
{
    auto* self = reinterpret_cast<void*>(&rdma_connect);
    auto* bound = ::dlsym(RTLD_DEFAULT, "rdma_connect");
    auto* real = ::dlsym(RTLD_NEXT, "rdma_connect");
    return bound == self && real != nullptr && real != self;
}
} // namespace mfa::qp
