#include <doctest/doctest.h>

#include "config/config.hpp"
#include "demand/derive.hpp"
#include "nmos/observer.hpp"
#include "util/cidr.hpp"

using namespace mfa;

IpCidr cidr(char const* text)
{
    auto parsed = parseIpCidr(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

TEST_CASE("local node matching")
{
    LocalIdentity self;
    self.hostname = "host-a";
    self.addresses = {"10.0.0.5", "127.0.0.1"};
    self.ownNodeId = "self";
    NodeView own;
    own.id = "self";
    own.hostname = "host-a";
    CHECK_FALSE(nodeIsLocal(own, self));
    NodeView byIp;
    byIp.id = "other";
    byIp.endpoints.push_back(EndpointRef{"10.0.0.5", 3212});
    CHECK(nodeIsLocal(byIp, self));
    NodeView byName;
    byName.id = "n2";
    byName.hostname = "host-a";
    CHECK(nodeIsLocal(byName, self));
    self.extraNodeIds.insert("explicit");
    NodeView extra;
    extra.id = "explicit";
    CHECK(nodeIsLocal(extra, self));
    NodeView remote;
    remote.id = "far";
    remote.hostname = "host-b";
    remote.endpoints.push_back(EndpointRef{"10.1.1.9", 80});
    CHECK_FALSE(nodeIsLocal(remote, self));
}

TEST_CASE("LOCAL_NODE_CIDRS matches endpoint addresses")
{
    LocalIdentity self;
    self.ownNodeId = "agent";
    self.cidrs = {cidr("10.42.3.0/24"), cidr("10.42.9.0/24"), cidr("fd00:42:3::/64")};

    NodeView inside;
    inside.id = "pod-a";
    inside.hostname = "decklink-0";
    inside.endpoints.push_back(EndpointRef{"10.42.3.15", 3212});
    auto const v4 = matchLocalNode(inside, self);
    CHECK(v4.local);
    CHECK(std::string(v4.rule) == "cidr");

    NodeView second;
    second.id = "pod-b";
    second.endpoints.push_back(EndpointRef{"10.42.9.5", 80});
    CHECK(matchLocalNode(second, self).local);

    NodeView outside;
    outside.id = "pod-c";
    outside.endpoints.push_back(EndpointRef{"10.42.4.1", 80});
    CHECK_FALSE(nodeIsLocal(outside, self));

    NodeView v6;
    v6.id = "pod-v6";
    v6.endpoints.push_back(EndpointRef{"fd00:42:3::10", 80});
    CHECK(std::string(matchLocalNode(v6, self).rule) == "cidr");

    NodeView v6out;
    v6out.id = "pod-v6-out";
    v6out.endpoints.push_back(EndpointRef{"fd00:42:4::10", 80});
    CHECK_FALSE(nodeIsLocal(v6out, self));

    NodeView own;
    own.id = "agent";
    own.endpoints.push_back(EndpointRef{"10.42.3.2", 3232});
    CHECK_FALSE(nodeIsLocal(own, self));
}

TEST_CASE("LOCAL_NODE_CIDRS resolves names only when configured")
{
    LocalIdentity self;
    int calls = 0;
    self.resolveName = [&](std::string const& name) {
        ++calls;
        if (name == "sport-sa-cc1.prod-sport-sa.svc.cluster.local")
        {
            return std::vector<std::string>{"10.42.3.20"};
        }
        if (name == "other.svc.cluster.local")
        {
            return std::vector<std::string>{"10.9.9.9"};
        }
        return std::vector<std::string>{};
    };

    NodeView named;
    named.id = "cc1";
    named.endpoints.push_back(EndpointRef{"sport-sa-cc1.prod-sport-sa.svc.cluster.local", 8080});
    CHECK_FALSE(nodeIsLocal(named, self));
    CHECK(calls == 0);

    self.cidrs = {cidr("10.42.3.0/24")};
    CHECK(std::string(matchLocalNode(named, self).rule) == "cidr");
    CHECK(calls == 1);

    NodeView other;
    other.id = "other";
    other.endpoints.push_back(EndpointRef{"other.svc.cluster.local", 80});
    CHECK_FALSE(nodeIsLocal(other, self));

    NodeView missing;
    missing.id = "missing";
    missing.endpoints.push_back(EndpointRef{"no-such.svc.cluster.local", 80});
    CHECK_FALSE(nodeIsLocal(missing, self));

    NodeView literal;
    literal.id = "lit";
    literal.endpoints.push_back(EndpointRef{"10.42.3.8", 80});
    int const before = calls;
    CHECK(nodeIsLocal(literal, self));
    CHECK(calls == before);
}

TEST_CASE("LOCAL_NODE_CIDRS config parsing")
{
    auto cfg = parseConfig({{"HOST_ID", "h"}, {"LOCAL_NODE_CIDRS", " 10.42.3.0/24, fd00:42:3::/64 "}});
    REQUIRE(cfg.local_node_cidrs.size() == 2);
    CHECK(cfg.local_node_cidrs[0].text == "10.42.3.0/24");
    CHECK(cfg.local_node_cidrs[1].prefix == 64);
    CHECK_FALSE(isRuntimeKey("LOCAL_NODE_CIDRS"));
    auto const exported = configToMap(cfg)["LOCAL_NODE_CIDRS"];
    CHECK(exported == "10.42.3.0/24,fd00:42:3::/64");

    CHECK_THROWS_AS(parseConfig({{"HOST_ID", "h"}, {"LOCAL_NODE_CIDRS", "10.42.3.0"}}), ConfigError);
    CHECK_THROWS_AS(parseConfig({{"HOST_ID", "h"}, {"LOCAL_NODE_CIDRS", "10.42.3.0/33"}}), ConfigError);
    CHECK_THROWS_AS(parseConfig({{"HOST_ID", "h"}, {"LOCAL_NODE_CIDRS", "not-a-cidr"}}), ConfigError);
    CHECK(parseConfig({{"HOST_ID", "h"}}).local_node_cidrs.empty());
}

TEST_CASE("active parsing and demand states")
{
    auto active = parseActive(R"({"master_enable":true,"sender_id":null,"transport_params":[{"mxl_domain_id":"dom","mxl_flow_id":"flow"}]})");
    REQUIRE(active.has_value());
    CHECK(active->master_enable);
    CHECK(active->domain_id == "dom");
    CHECK(active->flow_id == "flow");

    ReceiverView local;
    local.id = "rx-local";
    local.active.master_enable = true;
    local.active.domain_set = true;
    local.active.flow_set = true;
    local.active.domain_id = "local-dom";
    local.active.flow_id = "flow-l";

    ReceiverView remote;
    remote.id = "rx-remote";
    remote.active.master_enable = true;
    remote.active.domain_set = true;
    remote.active.flow_set = true;
    remote.active.domain_id = "remote-dom";
    remote.active.flow_id = "flow-r";

    ReceiverView missing;
    missing.id = "rx-miss";
    missing.active.master_enable = true;
    missing.active.domain_set = true;
    missing.active.flow_set = true;
    missing.active.domain_id = "remote-dom";
    missing.active.flow_id = "gone";

    ReceiverView unknown;
    unknown.id = "rx-unk";
    unknown.active.master_enable = true;
    unknown.active.domain_set = true;
    unknown.active.flow_set = true;
    unknown.active.domain_id = "nope";
    unknown.active.flow_id = "flow-x";

    auto snap = deriveDemand({local, remote, missing, unknown}, {"local-dom"}, {"remote-dom"}, {"remote-dom/flow-r"});
    CHECK(snap.receivers.size() == 4);
    REQUIRE(snap.entries.size() == 3);
    int replicating = 0;
    int stale = 0;
    int unresolved = 0;
    for (auto const& entry : snap.entries)
    {
        if (entry.state == "replicating")
        {
            ++replicating;
        }
        if (entry.state == "stale_reference")
        {
            ++stale;
        }
        if (entry.state == "unresolved")
        {
            ++unresolved;
        }
    }
    CHECK(replicating == 1);
    CHECK(stale == 1);
    CHECK(unresolved == 1);
}

TEST_CASE("receiver /active url from a control href with or without the trailing slash")
{
    // FlowXer and nmos-cpp advertise ".../v1.2/"; FlowXer before 9.16.34 answered "//single" with 404,
    // so the agent saw its receivers as not routed and replicated nothing (platform, 2026-10-07).
    CHECK(receiverActiveUrl("http://10.42.1.26:3252/x-nmos/connection/v1.2/", "r1") == "http://10.42.1.26:3252/x-nmos/connection/v1.2/single/receivers/r1/active");
    CHECK(receiverActiveUrl("http://h:1/x-nmos/connection/v1.1", "r2") == "http://h:1/x-nmos/connection/v1.1/single/receivers/r2/active");
    CHECK(receiverActiveUrl("http://h:1/x-nmos/connection/v1.2//", "r3") == "http://h:1/x-nmos/connection/v1.2/single/receivers/r3/active");
}
