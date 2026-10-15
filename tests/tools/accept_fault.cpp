// LD_PRELOAD fault injection for tests/integration/accept_fail.sh.
//
// After the first accept() of a connection on a local port in MFA_FAULT_PORTS ("lo-hi"), the next
// MFA_FAULT_COUNT (default 1) calls that create a descriptor for a queue's wait object (epoll_create,
// epoll_create1, eventfd, socketpair, pipe, pipe2) fail with EMFILE. MXL's RC target opens the completion
// queue and the event queue of an accepted connection right after the accept, so that open fails and the
// target's read throws, as on the platform when rdma_create_qp failed with EINVAL (verbs) for a peer that
// connected right after a restart. One shot: later accepts are left alone.
//
// libfabric calls these through the C library, so a preloaded library sees them. A binary with file
// capabilities ignores LD_PRELOAD (secure execution): use the build tree's agent, not the image's.

#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace
{
std::atomic<bool> armed{true};
std::atomic<int> failing{0};

template<typename F>
F real(char const* name)
{
    return reinterpret_cast<F>(::dlsym(RTLD_NEXT, name));
}

void say(char const* text)
{
    auto const n = ::write(2, text, std::strlen(text));
    (void)n;
}

void accepted(int fd)
{
    if (fd < 0 || !armed.load())
    {
        return;
    }
    char const* range = std::getenv("MFA_FAULT_PORTS");
    if (range == nullptr)
    {
        return;
    }
    char* end = nullptr;
    long const lo = std::strtol(range, &end, 10);
    long const hi = (end != nullptr && *end == '-') ? std::strtol(end + 1, nullptr, 10) : lo;
    sockaddr_storage local{};
    socklen_t len = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len) != 0)
    {
        return;
    }
    long port = 0;
    if (local.ss_family == AF_INET)
    {
        port = ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port);
    }
    else if (local.ss_family == AF_INET6)
    {
        port = ntohs(reinterpret_cast<sockaddr_in6*>(&local)->sin6_port);
    }
    if (port < lo || port > hi || !armed.exchange(false))
    {
        return;
    }
    char const* count = std::getenv("MFA_FAULT_COUNT");
    failing.store(count != nullptr ? std::atoi(count) : 1);
    say("{\"level\":\"warn\",\"event\":\"accept_fault_armed\",\"detail\":\"the next queue descriptors fail with EMFILE\"}\n");
}

bool fault()
{
    int n = failing.load();
    while (n > 0)
    {
        if (failing.compare_exchange_weak(n, n - 1))
        {
            errno = EMFILE;
            say("{\"level\":\"warn\",\"event\":\"accept_fault_injected\"}\n");
            return true;
        }
    }
    return false;
}
} // namespace

extern "C"
{
int accept(int fd, sockaddr* addr, socklen_t* len)
{
    static auto const next = real<int (*)(int, sockaddr*, socklen_t*)>("accept");
    int const r = next(fd, addr, len);
    accepted(r);
    return r;
}

int accept4(int fd, sockaddr* addr, socklen_t* len, int flags)
{
    static auto const next = real<int (*)(int, sockaddr*, socklen_t*, int)>("accept4");
    int const r = next(fd, addr, len, flags);
    accepted(r);
    return r;
}

int epoll_create(int size)
{
    static auto const next = real<int (*)(int)>("epoll_create");
    return fault() ? -1 : next(size);
}

int epoll_create1(int flags)
{
    static auto const next = real<int (*)(int)>("epoll_create1");
    return fault() ? -1 : next(flags);
}

int eventfd(unsigned int initval, int flags)
{
    static auto const next = real<int (*)(unsigned int, int)>("eventfd");
    return fault() ? -1 : next(initval, flags);
}

int socketpair(int domain, int type, int protocol, int sv[2])
{
    static auto const next = real<int (*)(int, int, int, int*)>("socketpair");
    return fault() ? -1 : next(domain, type, protocol, sv);
}

int pipe(int fds[2])
{
    static auto const next = real<int (*)(int*)>("pipe");
    return fault() ? -1 : next(fds);
}

int pipe2(int fds[2], int flags)
{
    static auto const next = real<int (*)(int*, int)>("pipe2");
    return fault() ? -1 : next(fds, flags);
}
}
