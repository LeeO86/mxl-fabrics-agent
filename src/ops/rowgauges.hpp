#pragma once

#include "demand/derive.hpp"
#include "ops/metrics.hpp"
#include "replication/engine.hpp"

#include <vector>

namespace mfa
{
// Gauges with one series per row (replication, demand state). Each call replaces the whole set of series,
// so the series of a replication that ended, or of a state no entry has any more, go away.
void setReplicationGauges(Metrics& metrics, std::vector<ReplicaView> const& rows, bool perFlow);
void setDemandGauges(Metrics& metrics, std::vector<DemandEntry> const& entries);
} // namespace mfa
