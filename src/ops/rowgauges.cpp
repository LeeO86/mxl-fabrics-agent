#include "ops/rowgauges.hpp"

#include <map>
#include <string>
#include <utility>

namespace mfa
{
namespace
{
using SeriesList = std::vector<std::pair<std::map<std::string, std::string>, double>>;
}

void setReplicationGauges(Metrics& metrics, std::vector<ReplicaView> const& rows, bool perFlow,
    std::set<std::pair<std::string, std::string>>& seenPairs)
{
    std::map<std::pair<std::string, std::string>, int> active; // role, provider → count
    SeriesList states;
    SeriesList lagGrains;
    SeriesList lagMs;
    SeriesList cqDepth;
    for (auto const& row : rows)
    {
        seenPairs.insert({row.role, row.provider});
        if (row.state == "active" || row.state == "pending")
        {
            active[{row.role, row.provider}] += 1;
        }
        if (perFlow)
        {
            states.push_back({{{"flow_id", row.flow_id}, {"peer", row.peer}, {"role", row.role}, {"state", row.state}}, 1});
            lagGrains.push_back({{{"flow_id", row.flow_id}, {"peer", row.peer}}, static_cast<double>(row.lag)});
            lagMs.push_back({{{"flow_id", row.flow_id}, {"peer", row.peer}}, row.lag_ms});
            cqDepth.push_back({{{"flow_id", row.flow_id}}, static_cast<double>(row.cq_depth)});
        }
    }
    SeriesList activeSeries;
    for (auto const& key : seenPairs)
    {
        activeSeries.push_back({{{"role", key.first}, {"provider", key.second}}, static_cast<double>(active[key])});
    }
    metrics.setGauges("replications_active", activeSeries);
    // One series per replication with its current state (1); a former state disappears.
    metrics.setGauges("replication_state", states);
    metrics.setGauges("replication_lag_grains", lagGrains);
    metrics.setGauges("replication_lag_ms", lagMs);
    metrics.setGauges("completion_queue_depth", cqDepth);
}

void setDemandGauges(Metrics& metrics, std::vector<DemandEntry> const& entries)
{
    // The states src/demand/derive.cpp sets, at 0 when no entry has them.
    std::map<std::string, int> byState{{"replicating", 0}, {"unresolved", 0}, {"stale_reference", 0}};
    for (auto const& entry : entries)
    {
        byState[entry.state] += 1;
    }
    SeriesList series;
    for (auto const& [state, count] : byState)
    {
        series.push_back({{{"state", state}}, static_cast<double>(count)});
    }
    metrics.setGauges("demand_entries", series);
}
} // namespace mfa
