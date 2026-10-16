#include "ops/lanemetrics.hpp"

#include <algorithm>

namespace mfa
{
namespace
{
char const* const kHistogramNames[] = {"replication_sample_age_seconds", "replication_delivery_gap_seconds", "sample_transfer_seconds",
    "grain_transfer_seconds", "fabric_thread_stall_seconds"};
char const* const kMaxNames[] = {"replication_sample_age_max_seconds", "sample_transfer_max_seconds"};
char const* const kCounterNames[] = {"replication_late_deliveries_total", "sample_transfer_stalls_total", "grain_transfer_stalls_total"};
} // namespace

void LaneMetrics::collect(LaneStats const& lane, bool perFlow, Histograms& histograms, Counters& counters,
    std::map<std::string, std::map<Labels, double>>* maxes)
{
    Labels flow;
    if (perFlow)
    {
        flow["flow_id"] = lane.flowId;
    }
    auto add = [&](char const* metric, Labels const& labels, LaneHistogram const& histogram) {
        if (histogram.count != 0)
        {
            histograms[metric][labels].merge(histogram);
        }
    };
    auto max = [&](char const* metric, Labels const& labels, double value, LaneHistogram const& histogram) {
        if (maxes != nullptr && histogram.count != 0)
        {
            auto& slot = (*maxes)[metric][labels];
            slot = std::max(slot, value);
        }
    };
    static char const* const stages[] = {"origin", "sent", "delivered"};
    for (std::size_t i = 0; i < 3; ++i)
    {
        auto labels = flow;
        labels["stage"] = stages[i];
        add("replication_sample_age_seconds", labels, lane.histograms[i]);
        max("replication_sample_age_max_seconds", labels, lane.maxes[i], lane.histograms[i]);
    }
    auto const gap = static_cast<std::size_t>(LaneMetric::DeliveryGap);
    add("replication_delivery_gap_seconds", flow, lane.histograms[gap]);
    // Continuous and (since 1.7.3) discrete destinations.
    if (lane.deliveries != 0 || lane.histograms[gap].count != 0)
    {
        counters["replication_late_deliveries_total"][flow] += static_cast<double>(lane.lateDeliveries);
    }
    auto const sample = static_cast<std::size_t>(LaneMetric::SampleTransfer);
    auto transferLabels = flow;
    transferLabels["provider"] = lane.provider;
    add("sample_transfer_seconds", transferLabels, lane.histograms[sample]);
    max("sample_transfer_max_seconds", transferLabels, lane.maxes[sample], lane.histograms[sample]);
    if (lane.histograms[sample].count != 0)
    {
        counters["sample_transfer_stalls_total"][transferLabels] += static_cast<double>(lane.sampleTransferStalls);
    }
    auto const grain = static_cast<std::size_t>(LaneMetric::GrainTransfer);
    add("grain_transfer_seconds", {{"provider", lane.provider}}, lane.histograms[grain]);
    if (lane.histograms[grain].count != 0)
    {
        counters["grain_transfer_stalls_total"][{{"provider", lane.provider}}] += static_cast<double>(lane.grainTransferStalls);
    }
    for (auto const& [task, histogram] : lane.stalls)
    {
        add("fabric_thread_stall_seconds", {{"task", task}}, histogram);
    }
}

void LaneMetrics::retire(std::vector<LaneStats> const& gone, bool perFlow)
{
    Histograms histograms;
    Counters counters;
    for (auto const& lane : gone)
    {
        collect(lane, perFlow, histograms, counters, nullptr);
    }
    // Only the summed series (no flow label) keep a total.
    for (auto const& [metric, series] : histograms)
    {
        for (auto const& [labels, histogram] : series)
        {
            if (labels.count("flow_id") == 0)
            {
                retiredHistograms_[metric][labels].merge(histogram);
            }
        }
    }
    for (auto const& [metric, series] : counters)
    {
        for (auto const& [labels, value] : series)
        {
            if (labels.count("flow_id") == 0)
            {
                retiredCounters_[metric][labels] += value;
            }
        }
    }
}

void LaneMetrics::render(Metrics& metrics, std::vector<LaneStats> const& live, bool perFlow) const
{
    Histograms histograms = retiredHistograms_;
    Counters counters = retiredCounters_;
    std::map<std::string, std::map<Labels, double>> maxes;
    for (auto const& lane : live)
    {
        collect(lane, perFlow, histograms, counters, &maxes);
    }
    for (auto const* metric : kHistogramNames)
    {
        std::vector<std::pair<Labels, LaneHistogram>> series;
        for (auto const& [labels, histogram] : histograms[metric])
        {
            series.emplace_back(labels, histogram);
        }
        metrics.setHistograms(metric, series);
    }
    for (auto const* metric : kMaxNames)
    {
        std::vector<std::pair<Labels, double>> series(maxes[metric].begin(), maxes[metric].end());
        metrics.setGauges(metric, series);
    }
    for (auto const* metric : kCounterNames)
    {
        auto const& series = counters[metric];
        metrics.setCounters(metric, {series.begin(), series.end()});
    }
}
} // namespace mfa
