#pragma once

#include <atomic>
#include <cmath>

namespace mfa::qp
{
// The RDMA ACK timeout of a queue pair is an exponent n: the sender retransmits when no ACK came for 4.096 us x 2^n.
// rdma_cm sets n = packet life time + 1 = 17 (537 ms) on RoCE, so every lost packet held its flow's transfers
// for about half a second (platform, 1.7.2). The agent sets n and the retry count on every connection of the
// verbs provider (qpcontrol.hpp).
inline constexpr int kDefaultAckTimeout = 14; // 67 ms
inline constexpr int kDefaultRetryCount = 7;
inline constexpr int kRdmaCmAckTimeout = 17; // what rdma_cm sets on RoCE without the option

inline double ackTimeoutSeconds(int exponent)
{
    return 4.096e-6 * std::ldexp(1.0, exponent);
}

struct Settings
{
    int ackTimeout = kDefaultAckTimeout; // 0: leave it to rdma_cm
    int retryCount = kDefaultRetryCount;
};

namespace detail
{
inline std::atomic<int>& ackTimeout()
{
    static std::atomic<int> value{kDefaultAckTimeout};
    return value;
}
inline std::atomic<int>& retryCount()
{
    static std::atomic<int> value{kDefaultRetryCount};
    return value;
}
} // namespace detail

// Set once at start (FABRIC_ACK_TIMEOUT, FABRIC_RETRY_COUNT), read on every connection.
inline void configure(Settings settings)
{
    detail::ackTimeout().store(settings.ackTimeout);
    detail::retryCount().store(settings.retryCount);
}

inline Settings settings()
{
    return Settings{detail::ackTimeout().load(), detail::retryCount().load()};
}

// The exponent the queue pairs get: the setting, or rdma_cm's 17 when it is left to rdma_cm.
inline int effectiveAckTimeout(Settings const& s)
{
    return s.ackTimeout != 0 ? s.ackTimeout : kRdmaCmAckTimeout;
}

// A transfer longer than half the ACK timeout most likely waited for a retransmission (sample_transfer_stalls_total).
inline double transferStallThresholdSeconds(Settings const& s)
{
    return ackTimeoutSeconds(effectiveAckTimeout(s)) / 2.0;
}
} // namespace mfa::qp
