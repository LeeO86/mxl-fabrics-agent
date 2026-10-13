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
} // namespace mfa
