#pragma once

#include "fabric/lanestats.hpp"
#include "ops/metrics.hpp"

#include <map>
#include <string>
#include <vector>

namespace mfa
{
// Turns the lanes' delivery timing into metrics. Series without a flow label (grain_transfer_seconds{provider},
// fabric_thread_stall_seconds{task}, and all of them with METRICS_PER_FLOW=false) sum several lanes, so the
// final counts of a lane that goes are kept in a running total: those series never decrease. A per-flow series
// goes with its flow's lane.
class LaneMetrics
{
public:
    // The final stats of lanes that went since the last call.
    void retire(std::vector<LaneStats> const& gone, bool perFlow);
    // Live lanes plus the retired totals into `metrics` (replacing these series).
    void render(Metrics& metrics, std::vector<LaneStats> const& live, bool perFlow) const;

private:
    using Labels = std::map<std::string, std::string>;
    using Histograms = std::map<std::string, std::map<Labels, LaneHistogram>>; // metric → labels → histogram
    static void collect(LaneStats const& lane, bool perFlow, Histograms& histograms, std::map<Labels, double>& late,
        std::map<std::string, std::map<Labels, double>>* maxes);

    Histograms retiredHistograms_;
    std::map<Labels, double> retiredLate_;
};
} // namespace mfa
