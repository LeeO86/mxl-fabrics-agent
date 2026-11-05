#pragma once

#include "demand/derive.hpp"
#include "ops/metrics.hpp"
#include "replication/engine.hpp"

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mfa
{
// Gauges with one series per row. Each call replaces the whole set of series: the per-replication series
// (replication_state, replication_lag_*, completion_queue_depth) of a replication that ended go away.
// The counts stay at 0 instead: replications_active keeps every role/provider pair in `seenPairs` (filled
// here, kept by the caller), demand_entries every known demand state.
void setReplicationGauges(Metrics& metrics, std::vector<ReplicaView> const& rows, bool perFlow,
    std::set<std::pair<std::string, std::string>>& seenPairs);
void setDemandGauges(Metrics& metrics, std::vector<DemandEntry> const& entries);
} // namespace mfa
