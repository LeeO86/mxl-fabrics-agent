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

// One destination mirror domain (mirror-<domain id>) as a whole: keep it or drop it.
struct MirrorDomainInput
{
    bool origin_local = false;     // the domain is an origin on this node (a function moved here)
    bool origin_on_peer = false;   // an up peer lists the domain in its inventory
    bool source_peer_down = false; // its source peer is known but down: wait, it is not an orphan
    bool demand = false;           // a receiver wants one of its flows, a replication is held, or eager mode
    bool idle_expired = false;     // no demand for MIRROR_IDLE_GRACE_S
};

struct MirrorDomainPlan
{
    bool remove = false; // drop it now, unless a reader holds it (removalBlocker)
    std::string state;   // active, idle, orphan, same_node
    std::string reason;  // why it goes: same_node, orphan, idle
};

MirrorDomainPlan planMirrorDomain(MirrorDomainInput const& in);

// One destination mirror flow on disk, in a mirror domain of this agent, that no up peer offers: its origin
// flow is gone while the rest of the domain may still be in use.
struct MirrorFlowInput
{
    bool offered = false;        // an up peer lists the flow: the per-flow plan (planMirror) applies
    bool source_up = false;      // the domain's source peer is known and up, so its inventory is the truth
    bool peers_known = false;    // 30 s after start and, with NMOS, while the registry answers
    bool grace_expired = false;  // the origin has been missing for MIRROR_IDLE_GRACE_S (0 = never)
};

struct MirrorFlowPlan
{
    bool missing = false; // the origin is missing: the grace clock runs (it stops and resets otherwise)
    bool remove = false;  // drop the flow now, unless a reader maps it (removalBlocker)
    std::string reason;   // why it goes: origin_missing
};

MirrorFlowPlan planMirrorFlow(MirrorFlowInput const& in);

// The origin flow was created again with another definition under the same flow id: the mirror's
// flow_def.json (the definition it was created with) and the origin's differ in more than the descriptive
// fields (label, description, tags, parents, version, source_id, device_id). Rate, format, media type and
// geometry decide the mirror's layout, so such a mirror is rebuilt. False when either is empty or not a
// JSON object (no decision without both).
bool flowDefinitionChanged(std::string const& mirrorDef, std::string const& originDef);

// POST /api/v1/mirrors/{domain_id}/{flow_id}/rebuild: the test hook that rebuilds one mirror flow.
struct MirrorRebuildRequest
{
    bool matched = false; // the path is a rebuild request
    std::string domain_id;
    std::string flow_id;
};

MirrorRebuildRequest parseMirrorRebuildPath(std::string const& path);

// The answer before anything is touched: 403 while MIRROR_ADMIN_REBUILD is off (whatever the ids),
// 400 for ids that are not UUIDs, 404 when this agent has no such mirror flow, 0 to go ahead.
int mirrorRebuildRefusal(bool enabled, MirrorRebuildRequest const& request, bool found);
} // namespace mfa
