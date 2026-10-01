#include <doctest/doctest.h>

#include "mirror/lifecycle.hpp"

using namespace mfa;

MirrorInput base()
{
    MirrorInput in;
    in.peer_up = true;
    in.origin_present = true;
    in.origin_active = true;
    in.included = true;
    in.space_ok = true;
    in.eager = true;
    return in;
}

TEST_CASE("eager mirror without demand stays idle")
{
    auto in = base();
    auto plan = planMirror(in);
    CHECK(plan.create);
    CHECK_FALSE(plan.replicate);
    CHECK(plan.state == "idle");
}

TEST_CASE("demand starts replication")
{
    auto in = base();
    in.raw_demand = true;
    in.hold_replication = true;
    auto plan = planMirror(in);
    CHECK(plan.create);
    CHECK(plan.replicate);
    CHECK(plan.state == "replicating");
}

TEST_CASE("on-demand waits for a receiver")
{
    auto in = base();
    in.eager = false;
    auto plan = planMirror(in);
    CHECK_FALSE(plan.create);
    CHECK(plan.state == "absent");
    in.raw_demand = true;
    in.hold_replication = true;
    plan = planMirror(in);
    CHECK(plan.create);
    CHECK(plan.replicate);
}

TEST_CASE("origin loss keeps the mirror through grace then orphans")
{
    auto in = base();
    in.exists = true;
    in.origin_active = false;
    in.raw_demand = true;
    in.grace_holding = true;
    auto plan = planMirror(in);
    CHECK_FALSE(plan.remove);
    CHECK(plan.state == "idle");
    in.grace_holding = false;
    in.grace_expired = true;
    plan = planMirror(in);
    CHECK(plan.state == "orphaned");
    CHECK_FALSE(plan.remove);
    in.raw_demand = false;
    plan = planMirror(in);
    CHECK(plan.remove);
}

TEST_CASE("peer down keeps an existing mirror")
{
    auto in = base();
    in.exists = true;
    in.peer_up = false;
    auto plan = planMirror(in);
    CHECK(plan.state == "peer_down");
    CHECK_FALSE(plan.remove);
    CHECK_FALSE(plan.replicate);
}

TEST_CASE("separate include and exclude filters")
{
    FlowFilter filter;
    filter.includeDomainsEmpty = false;
    filter.domainIncluded = false;
    CHECK_FALSE(flowIncluded(filter));
    filter.domainIncluded = true;
    filter.includeFlowsEmpty = false;
    filter.flowIncluded = true;
    CHECK(flowIncluded(filter));
    filter.flowExcluded = true;
    CHECK_FALSE(flowIncluded(filter));
}

TEST_CASE("insufficient space blocks creation")
{
    auto in = base();
    in.space_ok = false;
    auto plan = planMirror(in);
    CHECK_FALSE(plan.create);
    CHECK(plan.state == "insufficient_space");
}
