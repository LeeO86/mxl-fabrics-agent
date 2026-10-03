#include "demand/derive.hpp"

#include "util/jsonutil.hpp"

#include <map>

namespace mfa
{
namespace
{
LocalDecision decided(char const* rule)
{
    return LocalDecision{true, rule};
}
} // namespace

LocalDecision matchLocalNode(NodeView const& node, LocalIdentity const& self)
{
    if (!self.ownNodeId.empty() && node.id == self.ownNodeId)
    {
        return {};
    }
    if (self.extraNodeIds.count(node.id) != 0)
    {
        return decided("list");
    }
    if (!node.hostname.empty() && node.hostname == self.hostname)
    {
        return decided("hostname");
    }
    if ((!node.hostname.empty() && self.extraHostnames.count(node.hostname) != 0) || self.extraHostnames.count(node.id) != 0)
    {
        return decided("list");
    }
    for (auto const& ep : node.endpoints)
    {
        if (self.addresses.count(ep.host) != 0)
        {
            return decided("ip");
        }
    }
    if (self.cidrs.empty())
    {
        return {};
    }
    for (auto const& ep : node.endpoints)
    {
        if (ep.host.empty())
        {
            continue;
        }
        std::string literal;
        if (parseIpLiteral(ep.host, &literal))
        {
            if (ipInCidrs(literal, self.cidrs))
            {
                return decided("cidr");
            }
            continue;
        }
        if (!self.resolveName)
        {
            continue;
        }
        for (auto const& address : self.resolveName(ep.host))
        {
            if (ipInCidrs(address, self.cidrs))
            {
                return decided("cidr");
            }
        }
    }
    return {};
}

bool nodeIsLocal(NodeView const& node, LocalIdentity const& self)
{
    return matchLocalNode(node, self).local;
}

std::optional<ActiveParams> parseActive(std::string const& body)
{
    auto const root = json::parse(body);
    if (!root.is<picojson::object>())
    {
        return std::nullopt;
    }
    auto const& obj = root.get<picojson::object>();
    ActiveParams active;
    active.master_enable = json::asBool(obj, "master_enable").value_or(false);
    if (obj.count("sender_id") && obj.at("sender_id").is<std::string>())
    {
        active.sender_id = obj.at("sender_id").get<std::string>();
    }
    auto const it = obj.find("transport_params");
    if (it == obj.end() || !it->second.is<picojson::array>() || it->second.get<picojson::array>().empty())
    {
        return active;
    }
    auto const& first = it->second.get<picojson::array>().front();
    if (!first.is<picojson::object>())
    {
        return active;
    }
    auto const& params = first.get<picojson::object>();
    if (auto domain = json::asString(params, "mxl_domain_id"))
    {
        active.domain_id = *domain;
        active.domain_set = !domain->empty() && *domain != "auto";
    }
    if (auto flow = json::asString(params, "mxl_flow_id"))
    {
        active.flow_id = *flow;
        active.flow_set = !flow->empty() && *flow != "auto";
    }
    return active;
}

DemandSnapshot deriveDemand(std::vector<ReceiverView> const& receivers, std::set<std::string> const& localDomainIds,
    std::set<std::string> const& peerDomainIds, std::set<std::string> const& peerFlowKeys)
{
    DemandSnapshot snap;
    snap.receivers = receivers;
    std::map<std::string, DemandEntry> grouped;
    for (auto& receiver : snap.receivers)
    {
        auto const& active = receiver.active;
        if (active.domain_set && localDomainIds.count(active.domain_id) != 0)
        {
            continue;
        }
        if (!active.master_enable || !active.domain_set || !active.flow_set)
        {
            continue;
        }
        if (localDomainIds.count(active.domain_id) != 0)
        {
            continue;
        }
        auto const key = active.domain_id + "/" + active.flow_id;
        auto& entry = grouped[key];
        entry.domain_id = active.domain_id;
        entry.flow_id = active.flow_id;
        entry.receiver_ids.push_back(receiver.id);
        if (peerDomainIds.count(active.domain_id) == 0)
        {
            entry.state = "unresolved";
        }
        else if (peerFlowKeys.count(key) == 0)
        {
            entry.state = "stale_reference";
        }
        else
        {
            entry.state = "replicating";
        }
    }
    for (auto& [_, entry] : grouped)
    {
        snap.entries.push_back(std::move(entry));
    }
    return snap;
}
} // namespace mfa
