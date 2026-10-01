#include "util/threading.hpp"

#include "util/logging.hpp"

#include <pthread.h>
#include <sched.h>

#include <cstring>

namespace mfa
{
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
