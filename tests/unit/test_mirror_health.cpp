#include <doctest/doctest.h>

#include "config/config.hpp"
#include "inventory/model.hpp"
#include "inventory/scanner.hpp"
#include "mirror/health.hpp"
#include "peer/manager.hpp"

#include <unistd.h>

#include <filesystem>
#include <fstream>

using namespace mfa;
using namespace std::chrono_literals;

namespace
{
constexpr double kTai = 1'000'000.0; // TAI seconds; at 50 grains/s index 50,000,000 is "now"
constexpr std::uint64_t kNow = 50'000'000;
auto const t0 = std::chrono::steady_clock::time_point{} + 1h;

// A mirror of a 50 Hz flow whose origin is live, with a grain `ageMs` old (none when negative).
MirrorObservation live(double ageMs, std::uint64_t inode = 7)
{
    MirrorObservation obs;
    obs.domain_id = "d";
    obs.flow_id = "f";
    obs.peer = "host-01";
    obs.inode = inode;
    obs.rate_num = 50;
    obs.rate_den = 1;
    obs.tai_now = kTai;
    obs.expected = true;
    obs.origin_live = true;
    if (ageMs >= 0)
    {
        obs.head = kNow - static_cast<std::uint64_t>(ageMs / 20.0);
    }
    obs.origin_head = kNow;
    return obs;
}

std::vector<std::string> kinds(std::vector<MirrorEvent> const& events)
{
    std::vector<std::string> out;
    for (auto const& event : events)
    {
        out.push_back(event.kind);
    }
    return out;
}
} // namespace

TEST_CASE("mirror health: age of the newest grain from its index")
{
    auto const age = indexAgeMs(kNow - 10, 50, 1, kTai);
    REQUIRE(age.has_value());
    CHECK(*age == doctest::Approx(200.0));
    CHECK(*indexAgeMs(kNow + 5, 50, 1, kTai) == 0.0); // a grain ahead of the clock is not old
    // 48 kHz audio: 480 samples behind are 10 ms.
    CHECK(*indexAgeMs(48'000'000'000ULL - 480, 48000, 1, kTai) == doctest::Approx(10.0));
    CHECK_FALSE(indexAgeMs(1, 0, 1, kTai).has_value());
}

TEST_CASE("mirror health: live, stale after the threshold, recovered")
{
    MirrorHealthTracker tracker;
    CHECK(tracker.update(live(40), 1000, t0, 1000).empty());
    auto h = tracker.entries().at("d/f");
    CHECK(h.state == "live");
    REQUIRE(h.last_grain_age_ms.has_value());
    CHECK(*h.last_grain_age_ms == doctest::Approx(40.0));

    // No new grain: 900 ms is still within the threshold, 1000 ms is not.
    CHECK(tracker.update(live(900), 1000, t0 + 900ms, 1900).empty());
    CHECK(tracker.entries().at("d/f").state == "live");
    auto events = tracker.update(live(1000), 1000, t0 + 1000ms, 2000);
    CHECK(kinds(events) == std::vector<std::string>{"stale"});
    h = tracker.entries().at("d/f");
    CHECK(h.state == "stale");
    CHECK(h.reason == "no_new_grains");
    CHECK(h.stale_since_unix_ms == 2000);
    CHECK(tracker.update(live(1500), 1000, t0 + 1500ms, 2500).empty()); // one event per change

    events = tracker.update(live(20), 1000, t0 + 2000ms, 3000);
    REQUIRE(kinds(events) == std::vector<std::string>{"recovered"});
    CHECK(events[0].stale_ms == doctest::Approx(1000.0));
    CHECK(events[0].health.state == "live");
    CHECK(tracker.entries().at("d/f").stale_since_unix_ms == 0);
}

TEST_CASE("mirror health: no first grain")
{
    MirrorHealthTracker tracker;
    tracker.update(live(-1), 1000, t0, 0);
    CHECK(tracker.entries().at("d/f").state == "pending");
    CHECK_FALSE(tracker.entries().at("d/f").last_grain_age_ms.has_value());
    auto events = tracker.update(live(-1), 1000, t0 + 1s, 0);
    CHECK(kinds(events) == std::vector<std::string>{"stale"});
    CHECK(tracker.entries().at("d/f").reason == "no_first_grain");
}

TEST_CASE("mirror health: old grains get the full wait when grains become due")
{
    MirrorHealthTracker tracker;
    auto idle = live(60'000);
    idle.expected = false;
    idle.idle_reason = "no_demand"; // eager mirror, nobody reads it
    tracker.update(idle, 1000, t0, 0);
    CHECK(tracker.entries().at("d/f").state == "idle");
    CHECK(tracker.entries().at("d/f").reason == "no_demand");
    // A receiver is routed: the replication starts, the newest grain is a minute old.
    CHECK(tracker.update(live(60'000), 1000, t0 + 1s, 0).empty());
    CHECK(tracker.entries().at("d/f").state == "pending");
    CHECK(tracker.update(live(60'500), 1000, t0 + 1500ms, 0).empty());
    CHECK(kinds(tracker.update(live(61'000), 1000, t0 + 2s, 0)) == std::vector<std::string>{"stale"});
}

TEST_CASE("mirror health: an origin that writes nothing leaves the mirror quiet, not stale")
{
    MirrorHealthTracker tracker;
    tracker.update(live(20), 1000, t0, 0);
    auto silent = live(3000);
    silent.origin_head = *silent.head; // the source's head is the mirror's: nothing is missing
    CHECK(tracker.update(silent, 1000, t0 + 3s, 0).empty());
    CHECK(tracker.entries().at("d/f").state == "idle");
    CHECK(tracker.entries().at("d/f").reason == "origin_silent");
    // The origin writes again and the mirror does not follow.
    auto behind = live(4000);
    CHECK(kinds(tracker.update(behind, 1000, t0 + 4s, 0)) == std::vector<std::string>{"stale"});
}

TEST_CASE("mirror health: a source without head_index falls back to its live flag")
{
    MirrorHealthTracker tracker;
    auto obs = live(2000);
    obs.origin_head.reset();
    tracker.update(live(20), 1000, t0, 0);
    tracker.update(obs, 1000, t0 + 2s, 0);
    CHECK(tracker.entries().at("d/f").state == "stale");
    obs.origin_live = false;
    tracker.update(obs, 1000, t0 + 3s, 0);
    CHECK(tracker.entries().at("d/f").state == "idle");
}

TEST_CASE("mirror health: peer or link down is stale with that reason")
{
    MirrorHealthTracker tracker;
    tracker.update(live(20), 1000, t0, 0);
    CHECK(tracker.expected("d/f"));
    auto down = live(1200);
    down.origin_head.reset();
    down.origin_live = false;
    down.blocked = "link_down";
    auto events = tracker.update(down, 1000, t0 + 1200ms, 0);
    CHECK(kinds(events) == std::vector<std::string>{"stale"});
    CHECK(events[0].health.reason == "link_down");
}

TEST_CASE("mirror health: MIRROR_STALE_MS=0 never reports stale")
{
    MirrorHealthTracker tracker;
    tracker.update(live(20), 0, t0, 0);
    CHECK(tracker.update(live(60'000), 0, t0 + 60s, 0).empty());
    CHECK(tracker.entries().at("d/f").state == "live");
}

TEST_CASE("mirror health: a new data file is a rebuild")
{
    MirrorHealthTracker tracker;
    CHECK(tracker.update(live(20, 7), 1000, t0, 0).empty()); // first sight: no rebuild
    auto events = tracker.update(live(20, 8), 1000, t0 + 1s, 5000);
    REQUIRE(kinds(events) == std::vector<std::string>{"rebuilt"});
    CHECK(events[0].old_inode == 7);
    auto h = tracker.entries().at("d/f");
    CHECK(h.rebuilds == 1);
    CHECK(h.last_rebuild_unix_ms == 5000);
    CHECK(h.last_rebuild_reason == "replaced");

    // The agent removes it (origin gone), it is absent, then created again with the same inode number.
    tracker.noteRemoved("d/f", "origin_gone");
    tracker.update(live(20, 0), 1000, t0 + 2s, 6000);
    h = tracker.entries().at("d/f");
    CHECK(h.state == "absent");
    CHECK(h.reason == "origin_gone");
    CHECK_FALSE(tracker.expected("d/f"));
    events = tracker.update(live(20, 8), 1000, t0 + 3s, 7000);
    REQUIRE(kinds(events) == std::vector<std::string>{"rebuilt"});
    h = tracker.entries().at("d/f");
    CHECK(h.rebuilds == 2);
    CHECK(h.last_rebuild_reason == "origin_gone");
    CHECK(tracker.update(live(20, 8), 1000, t0 + 4s, 8000).empty()); // reason used once
    CHECK(tracker.entries().at("d/f").rebuilds == 2);
}

TEST_CASE("mirror health: a stale mirror that is rebuilt reports both")
{
    MirrorHealthTracker tracker;
    tracker.update(live(20, 7), 1000, t0, 0);
    tracker.update(live(1500, 7), 1000, t0 + 1500ms, 0);
    REQUIRE(tracker.entries().at("d/f").state == "stale");
    auto events = tracker.update(live(-1, 9), 1000, t0 + 2s, 0);
    CHECK(kinds(events) == std::vector<std::string>{"rebuilt", "recovered"});
    CHECK(tracker.entries().at("d/f").state == "pending"); // the new file waits for its first grain
}

TEST_CASE("mirror health: entries not seen for the keep time are forgotten")
{
    MirrorHealthTracker tracker;
    tracker.update(live(20), 1000, t0, 0);
    tracker.prune(t0 + 1h, 24h);
    CHECK(tracker.entries().count("d/f") == 1);
    tracker.prune(t0 + 25h, 24h);
    CHECK(tracker.entries().empty());
}

TEST_CASE("mirror health: MIRROR_STALE_MS")
{
    CHECK(parseConfig({}).mirror_stale_ms == 1000);
    CHECK(parseConfig({{"MIRROR_STALE_MS", "0"}}).mirror_stale_ms == 0);
    CHECK(parseConfig({{"MIRROR_STALE_MS", "250"}}).mirror_stale_ms == 250);
    CHECK_THROWS_AS(parseConfig({{"MIRROR_STALE_MS", "-1"}}), ConfigError);
    CHECK(isRuntimeKey("MIRROR_STALE_MS"));
}

TEST_CASE("inventory: head_index is optional and reaches the peer")
{
    Inventory inventory;
    inventory.host_id = "a";
    inventory.revision = 3;
    DomainRecord domain;
    domain.domain_id = "d";
    domain.kind = "local";
    FlowRecord flow;
    flow.flow_id = "f";
    flow.active = true;
    flow.live = true;
    domain.flows.push_back(flow);
    inventory.domains.push_back(domain);
    auto const without = inventory.canonicalJson();
    CHECK(without.find("head_index") == std::string::npos);
    CHECK_FALSE(parseInventoryJson(without).domains.at(0).flows.at(0).head_index.has_value());
    inventory.domains[0].flows[0].head_index = 89'123'456'789ULL;
    auto const parsed = parseInventoryJson(inventory.canonicalJson());
    REQUIRE(parsed.domains.at(0).flows.at(0).head_index.has_value());
    CHECK(*parsed.domains.at(0).flows.at(0).head_index == 89'123'456'789ULL);
}

TEST_CASE("flow header: inode of the data file, 0 when missing")
{
    auto const dir = std::filesystem::temp_directory_path() / ("mfa-header-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    auto const data = (dir / "data").string();
    CHECK(readFlowHeader(data).inode == 0);
    std::ofstream(data) << "short";
    auto const header = readFlowHeader(data);
    CHECK(header.inode != 0);
    CHECK(header.headIndex == 0); // too short for a version 1 header
    std::filesystem::remove_all(dir);
}
