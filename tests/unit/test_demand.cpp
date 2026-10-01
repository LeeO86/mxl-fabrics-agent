#include <doctest/doctest.h>

#include "demand/derive.hpp"

using namespace mfa;

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
