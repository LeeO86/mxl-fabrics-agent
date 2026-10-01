#pragma once

#include <string>

namespace mfa
{
struct MirrorInput
{
    bool exists = false;
    bool origin_present = false;
    bool origin_active = false;
    bool peer_up = false;
    bool eager = true;
    bool included = true;
    bool conflict = false;
    bool space_ok = true;
    bool raw_demand = false;
    bool hold_replication = false;
    bool grace_holding = false;
    bool grace_expired = false;
};

struct MirrorPlan
{
    bool create = false;
    bool remove = false;
    bool replicate = false;
    std::string state = "absent";
};

MirrorPlan planMirror(MirrorInput const& in);

struct FlowFilter
{
    bool includeDomainsEmpty = true;
    bool includeFlowsEmpty = true;
    bool domainIncluded = false;
    bool flowIncluded = false;
    bool domainExcluded = false;
    bool flowExcluded = false;
};

bool flowIncluded(FlowFilter const& filter);
} // namespace mfa
