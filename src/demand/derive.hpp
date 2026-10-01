#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mfa
{
struct EndpointRef
{
    std::string host;
    int port = 0;
};

struct NodeView
{
    std::string id;
    std::string hostname;
    std::string label;
    std::vector<EndpointRef> endpoints;
};

struct LocalIdentity
{
    std::string hostname;
    std::set<std::string> addresses;
    std::set<std::string> extraNodeIds;
    std::set<std::string> extraHostnames;
    std::string ownNodeId;
};

bool nodeIsLocal(NodeView const& node, LocalIdentity const& self);

struct ActiveParams
{
    bool master_enable = false;
    std::string domain_id;
    std::string flow_id;
    std::string sender_id;
    bool domain_set = false;
    bool flow_set = false;
};

std::optional<ActiveParams> parseActive(std::string const& json);

enum class ReceiverState
{
    Local,
    Replicating,
    Unresolved,
    StaleReference,
};

struct ReceiverView
{
    std::string id;
    std::string label;
    std::string node_id;
    std::string node_label;
    std::string device_id;
    std::string control_href;
    ActiveParams active;
};

struct DemandEntry
{
    std::string domain_id;
    std::string flow_id;
    std::string state; // replicating, unresolved, stale_reference
    std::string source_host;
    std::vector<std::string> receiver_ids;
};

struct DemandSnapshot
{
    std::vector<ReceiverView> receivers;
    std::vector<DemandEntry> entries;
};

// localDomainIds: domains this host owns (kind local, not conflict).
// peerDomains: domain id -> host id for domains exported by peers.
// peerFlows: "domain/flow" present in the latest peer inventory.
DemandSnapshot deriveDemand(std::vector<ReceiverView> const& receivers, std::set<std::string> const& localDomainIds,
    std::set<std::string> const& peerDomainIds, std::set<std::string> const& peerFlowKeys);
} // namespace mfa
