#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mfa
{
// A histogram counted on one lane's thread (no lock): bucket i counts values <= bounds[i].
struct LaneHistogram
{
    std::vector<double> bounds;
    std::vector<std::uint64_t> buckets;
    std::uint64_t count = 0;
    double sum = 0;

    void observe(double value)
    {
        if (buckets.size() != bounds.size())
        {
            buckets.assign(bounds.size(), 0);
        }
        count += 1;
        sum += value;
        for (std::size_t i = 0; i < bounds.size(); ++i)
        {
            if (value <= bounds[i])
            {
                buckets[i] += 1;
            }
        }
    }

    // Adds another histogram with the same bounds (aggregation without per-flow labels).
    void merge(LaneHistogram const& other)
    {
        if (bounds.empty())
        {
            *this = other;
            return;
        }
        if (other.bounds != bounds)
        {
            return;
        }
        if (buckets.size() != bounds.size())
        {
            buckets.assign(bounds.size(), 0);
        }
        for (std::size_t i = 0; i < bounds.size() && i < other.buckets.size(); ++i)
        {
            buckets[i] += other.buckets[i];
        }
        count += other.count;
        sum += other.sum;
    }
};

// The largest value of the last `kSeconds` seconds, kept per second (no reset on read, so several
// scrapers see the same value).
class RollingMax
{
public:
    static constexpr int kSeconds = 10;

    void observe(double value, std::int64_t second)
    {
        auto& slot = slots_[static_cast<std::size_t>(((second % kSeconds) + kSeconds) % kSeconds)];
        if (slot.second != second)
        {
            slot.second = second;
            slot.max = value;
            return;
        }
        if (value > slot.max)
        {
            slot.max = value;
        }
    }

    // 0 when nothing was observed in the window.
    double value(std::int64_t second) const
    {
        double out = 0;
        for (auto const& slot : slots_)
        {
            if (slot.second > second - kSeconds && slot.second <= second && slot.max > out)
            {
                out = slot.max;
            }
        }
        return out;
    }

private:
    struct Slot
    {
        std::int64_t second = -1000000;
        double max = 0;
    };
    std::array<Slot, kSeconds> slots_{};
};

// What a lane counts (an index, so the lane thread builds no key per observation).
enum class LaneMetric : std::size_t
{
    SampleAgeOrigin,    // continuous source: the origin head when a transfer is posted
    SampleAgeSent,      // continuous source: the newest sample of a transfer when it is posted
    SampleAgeDelivered, // continuous destination: the newest sample of a batch when it is committed
    DeliveryGap,        // continuous destination: time between two passes that committed samples
    SampleTransfer,     // continuous source: one transfer from post to completion
    GrainTransfer,      // discrete source: one grain from transfer to completion
    Count
};
inline constexpr std::size_t kLaneMetrics = static_cast<std::size_t>(LaneMetric::Count);

// Delivery timing of one lane, counted on its thread and handed out as a copy.
struct LaneStats
{
    std::string flowId;
    std::string provider; // of the source's initiator (sample and grain transfers)
    std::array<LaneHistogram, kLaneMetrics> histograms{};
    std::array<double, kLaneMetrics> maxes{}; // rolling maximum of the last RollingMax::kSeconds
    std::map<std::string, LaneHistogram> stalls; // by the task that held the thread ("pass": none)
    std::uint64_t lateDeliveries = 0;    // delivery gaps that lateGap counts
};

// A delivery gap counts as late when it is longer than twice the lane's usual gap (a moving average of
// the gaps) and at least 10 ms: about 6 ms with 3 ms transfers; 20 ms with 1 ms commits.
inline bool lateGap(double gapSeconds, double usualSeconds)
{
    auto const threshold = usualSeconds * 2.0 > 0.01 ? usualSeconds * 2.0 : 0.01;
    return gapSeconds > threshold;
}

// Bucket bounds (seconds). The default ones are those of grain_transfer_seconds since 1.0.
std::vector<double> const& laneBounds(LaneMetric metric);
inline std::vector<double> const& defaultBounds()
{
    static std::vector<double> const bounds{0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 5.0};
    return bounds;
}

inline std::vector<double> const& sampleAgeBounds()
{
    static std::vector<double> const bounds{0.002, 0.005, 0.01, 0.02, 0.04, 0.06, 0.08, 0.1, 0.12, 0.2, 0.5, 1.0};
    return bounds;
}

inline std::vector<double> const& deliveryGapBounds()
{
    static std::vector<double> const bounds{0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0};
    return bounds;
}

inline std::vector<double> const& sampleTransferBounds()
{
    static std::vector<double> const bounds{0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0};
    return bounds;
}

inline std::vector<double> const& stallBounds()
{
    static std::vector<double> const bounds{0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0};
    return bounds;
}
inline std::vector<double> const& laneBounds(LaneMetric metric)
{
    switch (metric)
    {
    case LaneMetric::SampleAgeOrigin:
    case LaneMetric::SampleAgeSent:
    case LaneMetric::SampleAgeDelivered: return sampleAgeBounds();
    case LaneMetric::DeliveryGap: return deliveryGapBounds();
    case LaneMetric::SampleTransfer: return sampleTransferBounds();
    default: return defaultBounds();
    }
}
} // namespace mfa
