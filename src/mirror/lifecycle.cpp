#include "mirror/lifecycle.hpp"

#include "util/jsonutil.hpp"
#include "util/uuid.hpp"

#include <optional>

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

MirrorDomainPlan planMirrorDomain(MirrorDomainInput const& in)
{
    MirrorDomainPlan plan;
    if (in.origin_local)
    {
        // A frozen copy next to the live origin: readers must find the origin.
        plan.state = "same_node";
        plan.reason = "same_node";
    }
    else if (!in.origin_on_peer && !in.source_peer_down)
    {
        plan.state = "orphan";
        plan.reason = "orphan";
    }
    else if (in.demand)
    {
        plan.state = "active";
        return plan;
    }
    else
    {
        plan.state = "idle";
        if (!in.idle_expired)
        {
            return plan;
        }
        plan.reason = "idle";
    }
    plan.remove = true;
    return plan;
}

namespace
{
// A flow definition without its descriptive fields, serialized with sorted keys; none when it is not a JSON object.
std::optional<std::string> layoutOf(std::string const& def)
{
    if (def.empty())
    {
        return std::nullopt;
    }
    std::string error;
    auto root = json::parse(def, &error);
    if (!error.empty() || !root.is<picojson::object>())
    {
        return std::nullopt;
    }
    auto& obj = root.get<picojson::object>();
    for (char const* key : {"label", "description", "tags", "parents", "version", "source_id", "device_id"})
    {
        obj.erase(key);
    }
    return root.serialize();
}
} // namespace

bool flowDefinitionChanged(std::string const& mirrorDef, std::string const& originDef)
{
    auto const mirror = layoutOf(mirrorDef);
    auto const origin = layoutOf(originDef);
    return mirror && origin && *mirror != *origin;
}

MirrorRebuildRequest parseMirrorRebuildPath(std::string const& path)
{
    static std::string const prefix = "/api/v1/mirrors/";
    static std::string const suffix = "/rebuild";
    MirrorRebuildRequest request;
    if (path.size() <= prefix.size() + suffix.size() || path.compare(0, prefix.size(), prefix) != 0 ||
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0)
    {
        return request;
    }
    auto const ids = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
    auto const slash = ids.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == ids.size() || ids.find('/', slash + 1) != std::string::npos)
    {
        return request;
    }
    request.matched = true;
    request.domain_id = ids.substr(0, slash);
    request.flow_id = ids.substr(slash + 1);
    return request;
}

int mirrorRebuildRefusal(bool enabled, MirrorRebuildRequest const& request, bool found)
{
    if (!enabled)
    {
        return 403;
    }
    if (!parseUuid(request.domain_id) || !parseUuid(request.flow_id))
    {
        return 400;
    }
    return found ? 0 : 404;
}

MirrorFlowPlan planMirrorFlow(MirrorFlowInput const& in)
{
    MirrorFlowPlan plan;
    // A source peer that is down may still hold the origin: wait for it, as for a whole domain. An
    // inventory that is not settled may not list the flow yet: no clock, no log until it is.
    plan.missing = !in.offered && in.source_up && in.inventories_settled;
    if (plan.missing && in.peers_known && in.grace_expired)
    {
        plan.remove = true;
        plan.reason = "origin_missing";
    }
    return plan;
}

std::string unofferedMirrorReason(bool sourceUp, bool inventoriesSettled)
{
    if (!sourceUp)
    {
        return "peer_down";
    }
    return inventoriesSettled ? "origin_missing" : "inventory_pending";
}
} // namespace mfa
