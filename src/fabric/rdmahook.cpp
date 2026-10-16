// librdmacm entry points of the agent binary (see fabric/qpcontrol.hpp). libfabric's verbs provider calls these
// through the dynamic linker, which finds the executable's definitions first; they call librdmacm's own.
// CMakeLists.txt exports them from the executable (--export-dynamic-symbol).

#include "fabric/qpcontrol.hpp"

#include <dlfcn.h>

#include <cerrno>
#include <utility>

namespace
{
// A symbol of `library` behind the binary's own: the next definition in the global scope, or, when the library was
// loaded with dlopen (RTLD_LOCAL, as a provider plugin would), from its own handle.
void* lookup(char const* library, char const* name, void* scope)
{
    if (auto* symbol = ::dlsym(scope, name); symbol != nullptr)
    {
        return symbol;
    }
    auto* handle = ::dlopen(library, RTLD_LAZY | RTLD_NOLOAD);
    if (handle == nullptr)
    {
        return nullptr;
    }
    auto* symbol = ::dlsym(handle, name);
    ::dlclose(handle); // drops the reference NOLOAD took; the library stays loaded
    return symbol;
}

template <typename F>
F rdmacm(char const* name)
{
    return reinterpret_cast<F>(lookup("librdmacm.so.1", name, RTLD_NEXT));
}

// Never destroyed: libfabric may call rdma_cm while static objects are torn down after main returns.
mfa::qp::Hooks& hooks()
{
    static auto* instance = [] {
        mfa::qp::Ops ops;
        ops.setOption = rdmacm<decltype(ops.setOption)>("rdma_set_option");
        ops.connect = rdmacm<decltype(ops.connect)>("rdma_connect");
        ops.accept = rdmacm<decltype(ops.accept)>("rdma_accept");
        ops.getEvent = rdmacm<decltype(ops.getEvent)>("rdma_get_cm_event");
        ops.destroyId = rdmacm<decltype(ops.destroyId)>("rdma_destroy_id");
        ops.destroyEp = rdmacm<decltype(ops.destroyEp)>("rdma_destroy_ep");
        // libibverbs is loaded with librdmacm (libfabric needs both).
        ops.queryQp = reinterpret_cast<decltype(ops.queryQp)>(lookup("libibverbs.so.1", "ibv_query_qp", RTLD_DEFAULT));
        return new mfa::qp::Hooks(ops, &mfa::qp::record);
    }();
    return *instance;
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

// librdmacm declares it void.
void rdma_destroy_ep(struct rdma_cm_id* id)
{
    hooks().destroyEp(id);
}
}

namespace mfa::qp
{
// The dynamic linker hands out the binary's hooks, and librdmacm's own are behind them (the startup log, lane-tests).
bool hooksResolved()
{
    std::pair<char const*, void*> const hooked[] = {{"rdma_connect", reinterpret_cast<void*>(&rdma_connect)},
        {"rdma_accept", reinterpret_cast<void*>(&rdma_accept)}, {"rdma_get_cm_event", reinterpret_cast<void*>(&rdma_get_cm_event)},
        {"rdma_destroy_id", reinterpret_cast<void*>(&rdma_destroy_id)}, {"rdma_destroy_ep", reinterpret_cast<void*>(&rdma_destroy_ep)}};
    for (auto const& [name, self] : hooked)
    {
        auto* real = lookup("librdmacm.so.1", name, RTLD_NEXT);
        if (::dlsym(RTLD_DEFAULT, name) != self || real == nullptr || real == self)
        {
            return false;
        }
    }
    return true;
}
} // namespace mfa::qp
