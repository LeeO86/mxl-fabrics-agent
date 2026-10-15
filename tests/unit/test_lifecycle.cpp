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

namespace
{
std::string videoDef(int rate, std::string const& label = "cam 1")
{
    return R"({"id":"b2b2b2b2-b2b2-4b2b-8b2b-b2b2b2b2b2b2","label":")" + label +
           R"(","description":"","tags":{},"parents":[],"version":"1:2","format":"urn:x-nmos:format:video",)"
           R"("media_type":"video/v210","grain_rate":{"numerator":)" +
           std::to_string(rate) + R"(,"denominator":1},"frame_width":1920,"frame_height":1080,"interlace_mode":"progressive"})";
}
} // namespace

TEST_CASE("origin created again: the same definition keeps the mirror")
{
    CHECK_FALSE(flowDefinitionChanged(videoDef(50), videoDef(50)));
    // Descriptive fields do not change the layout.
    CHECK_FALSE(flowDefinitionChanged(videoDef(50, "cam 1"), videoDef(50, "camera one")));
    auto reordered = std::string(R"({"frame_height":1080,"frame_width":1920,"interlace_mode":"progressive",)") +
                     R"("grain_rate":{"denominator":1,"numerator":50},"media_type":"video/v210","format":"urn:x-nmos:format:video",)" +
                     R"("id":"b2b2b2b2-b2b2-4b2b-8b2b-b2b2b2b2b2b2"})";
    CHECK_FALSE(flowDefinitionChanged(videoDef(50), reordered));
}

TEST_CASE("origin created again: another rate, size or format rebuilds the mirror")
{
    CHECK(flowDefinitionChanged(videoDef(50), videoDef(25)));
    auto other = videoDef(50);
    other.replace(other.find("1080,\"interlace"), 4, "720");
    CHECK(flowDefinitionChanged(videoDef(50), other));
    auto audio = videoDef(50);
    audio.replace(audio.find("video/v210"), 10, "audio/float32");
    CHECK(flowDefinitionChanged(videoDef(50), audio));
}

TEST_CASE("origin definition: no decision without both")
{
    CHECK_FALSE(flowDefinitionChanged("", videoDef(50)));
    CHECK_FALSE(flowDefinitionChanged(videoDef(50), ""));
    CHECK_FALSE(flowDefinitionChanged("{not json", videoDef(25)));
    CHECK_FALSE(flowDefinitionChanged("[1,2]", videoDef(25)));
}

TEST_CASE("rebuild hook: path")
{
    auto request = parseMirrorRebuildPath("/api/v1/mirrors/b1b1b1b1-b1b1-4b1b-8b1b-b1b1b1b1b1b1/b2b2b2b2-b2b2-4b2b-8b2b-b2b2b2b2b2b2/rebuild");
    REQUIRE(request.matched);
    CHECK(request.domain_id == "b1b1b1b1-b1b1-4b1b-8b1b-b1b1b1b1b1b1");
    CHECK(request.flow_id == "b2b2b2b2-b2b2-4b2b-8b2b-b2b2b2b2b2b2");
    CHECK_FALSE(parseMirrorRebuildPath("/api/v1/mirrors/d/rebuild").matched);
    CHECK_FALSE(parseMirrorRebuildPath("/api/v1/mirrors//f/rebuild").matched);
    CHECK_FALSE(parseMirrorRebuildPath("/api/v1/mirrors/d/f/x/rebuild").matched);
    CHECK_FALSE(parseMirrorRebuildPath("/api/v1/mirrors/d/f").matched);
    CHECK_FALSE(parseMirrorRebuildPath("/api/v1/replications/d/f/rebuild").matched);
}

TEST_CASE("rebuild hook: off unless enabled, then ids and the flow")
{
    auto const good = parseMirrorRebuildPath("/api/v1/mirrors/b1b1b1b1-b1b1-4b1b-8b1b-b1b1b1b1b1b1/b2b2b2b2-b2b2-4b2b-8b2b-b2b2b2b2b2b2/rebuild");
    auto const bad = parseMirrorRebuildPath("/api/v1/mirrors/..%2F..%2Fetc/passwd/rebuild");
    REQUIRE(bad.matched);
    // Off: 403 whatever is asked, before anything is looked at.
    CHECK(mirrorRebuildRefusal(false, good, true) == 403);
    CHECK(mirrorRebuildRefusal(false, bad, false) == 403);
    CHECK(mirrorRebuildRefusal(true, bad, false) == 400);
    CHECK(mirrorRebuildRefusal(true, good, false) == 404);
    CHECK(mirrorRebuildRefusal(true, good, true) == 0);
}
