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

// A transfer that takes longer than this most likely waited for a retransmission (sample_transfer_stalls_total,
// grain_transfer_stalls_total). Fixed, not derived from FABRIC_ACK_TIMEOUT: E810's irdma ignores the QP timeout and
// retransmits after its device-wide roce_rtomin (small: 5, about 54 ms; 50, about 540 ms, when PFC is detected), and
// mlx5 after at least the QP timeout (14: 67 ms). A retransmission adds that wait to the transfer's own time.
// Audio: a normal transfer takes about 2 to 4 ms (lab, tcp).
inline constexpr double kSampleTransferStallSeconds = 0.025;
// Video: the largest grain (2160p v210, 22 MB) takes about 7 ms over verbs at 25 Gb/s and 16 ms on average over the
// lab's tcp loopback (none over 40 ms in 60 s; see CHANGELOG 1.7.3).
inline constexpr double kGrainTransferStallSeconds = 0.040;

} // namespace mfa::qp
