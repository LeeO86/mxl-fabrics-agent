#include "mirror/lifecycle.hpp"

namespace mfa
{
bool flowIncluded(FlowFilter const& filter)
{
    if (filter.domainExcluded || filter.flowExcluded)
    {
        return false;
    }
    if (!filter.includeDomainsEmpty && !filter.domainIncluded)
    {
        return false;
    }
    if (!filter.includeFlowsEmpty && !filter.flowIncluded)
    {
        return false;
    }
    return true;
}

MirrorPlan planMirror(MirrorInput const& in)
{
    MirrorPlan plan;
    if (in.conflict)
    {
        plan.state = "conflict";
        return plan;
    }
    if (!in.included)
    {
        plan.remove = in.exists;
        plan.state = "absent";
        return plan;
    }
    if (!in.peer_up)
    {
        if (in.exists)
        {
            plan.state = "peer_down";
        }
        return plan;
    }
    bool const originLive = in.origin_present && in.origin_active;
    if (!originLive)
    {
        if (!in.exists)
        {
            if (!in.space_ok && (in.eager || in.raw_demand))
            {
                plan.state = "insufficient_space";
            }
            return plan;
        }
        if (in.raw_demand && in.grace_expired)
        {
            plan.state = "orphaned";
            return plan;
        }
        if (in.grace_expired && !in.raw_demand)
        {
            plan.remove = true;
            plan.state = "absent";
            return plan;
        }
        plan.state = in.grace_holding || in.raw_demand ? "idle" : "idle";
        return plan;
    }
    bool const wantMirror = in.eager || in.raw_demand || in.hold_replication;
    if (!wantMirror)
    {
        if (in.exists)
        {
            plan.remove = true;
        }
        return plan;
    }
    if (!in.space_ok && !in.exists)
    {
        plan.state = "insufficient_space";
        return plan;
    }
    if (!in.space_ok)
    {
        plan.state = "insufficient_space";
        return plan;
    }
    plan.create = true;
    plan.replicate = in.hold_replication && in.origin_active;
    plan.state = plan.replicate ? "replicating" : "idle";
    return plan;
}
} // namespace mfa
