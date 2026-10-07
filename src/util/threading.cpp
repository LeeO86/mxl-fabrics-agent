#include "util/threading.hpp"

#include "util/logging.hpp"

#include <linux/capability.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace mfa
{
std::string raisePermittedCapabilities()
{
    __user_cap_header_struct header{_LINUX_CAPABILITY_VERSION_3, 0};
    __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3]{};
    if (syscall(SYS_capget, &header, data) != 0)
    {
        return "unknown";
    }
    bool raise = false;
    for (auto& d : data)
    {
        raise = raise || (d.permitted & ~d.effective) != 0;
        d.effective |= d.permitted;
    }
    if (raise && syscall(SYS_capset, &header, data) != 0)
    {
        log::warn("capabilities_not_raised", {{"error", std::strerror(errno)}});
    }
    syscall(SYS_capget, &header, data);
    auto const has = [&](int cap) { return (data[cap / 32].effective & (1U << (cap % 32))) != 0; };
    std::string effective;
    if (has(CAP_SYS_NICE))
    {
        effective += "sys_nice";
    }
    if (has(CAP_IPC_LOCK))
    {
        effective += effective.empty() ? "ipc_lock" : ",ipc_lock";
    }
    return effective.empty() ? "none" : effective;
}

void applyThreadScheduling(int rtPriority, std::vector<int> const& cpus)
{
    if (!cpus.empty())
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int cpu : cpus)
        {
            CPU_SET(cpu, &set);
        }
        auto const rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
        if (rc != 0)
        {
            log::warn("cpu_affinity_failed", {{"error", std::strerror(rc)}});
        }
    }
    if (rtPriority > 0)
    {
        sched_param param{};
        param.sched_priority = rtPriority;
        auto const rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
        if (rc != 0)
        {
            log::warn("rt_priority_failed", {{"error", std::strerror(rc)}, {"priority", std::to_string(rtPriority)}});
        }
    }
}
} // namespace mfa
