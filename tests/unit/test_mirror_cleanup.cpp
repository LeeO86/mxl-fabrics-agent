#include <doctest/doctest.h>

#include "config/config.hpp"
#include "config/store.hpp"
#include "mirror/lifecycle.hpp"
#include "mirror/readers.hpp"
#include "peer/manager.hpp"

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace mfa;
namespace fs = std::filesystem;

namespace
{
MirrorDomainInput liveOrigin()
{
    MirrorDomainInput in;
    in.origin_on_peer = true;
    return in;
}

fs::path tempDir(std::string const& name)
{
    auto const dir = fs::temp_directory_path() / (name + "-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write(fs::path const& path, std::string const& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}

std::string mapsLine(fs::path const& file)
{
    struct stat st{};
    REQUIRE(::stat(file.c_str(), &st) == 0);
    char dev[32];
    std::snprintf(dev, sizeof dev, "%02x:%02x", major(st.st_dev), minor(st.st_dev));
    return std::string("7f0000000000-7f0000001000 rw-s 00000000 ") + dev + " " + std::to_string(st.st_ino) + "  " + file.string() + "\n";
}
} // namespace

TEST_CASE("mirror domain: same node goes first, even with demand")
{
    auto in = liveOrigin();
    in.origin_local = true;
    in.demand = true;
    auto const plan = planMirrorDomain(in);
    CHECK(plan.state == "same_node");
    CHECK(plan.reason == "same_node");
    CHECK(plan.remove);
}

TEST_CASE("mirror domain: orphan when no peer holds the origin")
{
    MirrorDomainInput in;
    in.demand = true;
    auto const plan = planMirrorDomain(in);
    CHECK(plan.state == "orphan");
    CHECK(plan.remove);
}

TEST_CASE("mirror domain: a source peer that is down is not an orphan")
{
    MirrorDomainInput in;
    in.source_peer_down = true;
    in.demand = true;
    auto plan = planMirrorDomain(in);
    CHECK(plan.state == "active");
    CHECK_FALSE(plan.remove);
    in.demand = false;
    in.idle_expired = true;
    plan = planMirrorDomain(in);
    CHECK(plan.state == "idle");
    CHECK(plan.remove);
}

TEST_CASE("mirror domain: active while there is demand")
{
    auto in = liveOrigin();
    in.demand = true;
    auto const plan = planMirrorDomain(in);
    CHECK(plan.state == "active");
    CHECK_FALSE(plan.remove);
}

TEST_CASE("mirror domain: idle is kept for the grace time, then removed")
{
    auto in = liveOrigin();
    auto plan = planMirrorDomain(in);
    CHECK(plan.state == "idle");
    CHECK_FALSE(plan.remove);
    in.idle_expired = true;
    plan = planMirrorDomain(in);
    CHECK(plan.state == "idle");
    CHECK(plan.reason == "idle");
    CHECK(plan.remove);
}

TEST_CASE("mirror domain: a reader refuses the removal")
{
    ReaderScan scan;
    std::vector<ReaderProcess> readers;
    CHECK(removalBlocker(scan, "/mxl/mirror-d1", &readers).empty());
    CHECK(readers.empty());
    scan.readers["/mxl/mirror-d1"] = {ReaderProcess{4242, "vmix"}};
    CHECK(removalBlocker(scan, "/mxl/mirror-d1", &readers) == "mapped by 4242(vmix)");
    REQUIRE(readers.size() == 1);
    CHECK(readers[0].pid == 4242);
    readers.clear();
    CHECK(removalBlocker(scan, "/mxl/mirror-d2", &readers).empty()); // another mirror is free
    scan.complete = false; // processes the agent cannot see: refuse every removal
    scan.error = "pid namespace";
    CHECK(removalBlocker(scan, "/mxl/mirror-d2", &readers) == "reader check incomplete: pid namespace");
}

TEST_CASE("mirror flow: offered flows are left to the per-flow plan")
{
    MirrorFlowInput in;
    in.offered = true;
    in.source_up = true;
    in.peers_known = true;
    in.grace_expired = true;
    auto const plan = planMirrorFlow(in);
    CHECK_FALSE(plan.missing);
    CHECK_FALSE(plan.remove);
}

TEST_CASE("mirror flow: a source peer that is down may still hold the origin")
{
    MirrorFlowInput in;
    in.peers_known = true;
    in.grace_expired = true;
    auto const plan = planMirrorFlow(in);
    CHECK_FALSE(plan.missing);
    CHECK_FALSE(plan.remove);
}

TEST_CASE("mirror flow: origin missing is kept for the grace time, then removed")
{
    MirrorFlowInput in;
    in.source_up = true;
    in.peers_known = true;
    in.inventories_settled = true;
    auto plan = planMirrorFlow(in);
    CHECK(plan.missing);
    CHECK_FALSE(plan.remove);
    in.grace_expired = true;
    plan = planMirrorFlow(in);
    CHECK(plan.missing);
    CHECK(plan.remove);
    CHECK(plan.reason == "origin_missing");
}

TEST_CASE("mirror flow: nothing goes before the peers are known")
{
    MirrorFlowInput in;
    in.source_up = true;
    in.inventories_settled = true;
    in.grace_expired = true;
    auto const plan = planMirrorFlow(in);
    CHECK(plan.missing); // the grace time runs meanwhile
    CHECK_FALSE(plan.remove);
}

TEST_CASE("mirror flow: not missing before every up peer's inventory is settled")
{
    // 1.7.1: all agents restarted together; 3 s after start the source's first inventory did not list a live
    // flow yet. No clock, no mirror_flow_origin_missing, no origin_missing health until it is settled.
    MirrorFlowInput in;
    in.source_up = true;
    in.peers_known = true;
    in.grace_expired = true;
    auto plan = planMirrorFlow(in);
    CHECK_FALSE(plan.missing);
    CHECK_FALSE(plan.remove);
    CHECK(unofferedMirrorReason(true, false) == "inventory_pending");
    CHECK(plan.pending);
    in.inventories_settled = true;
    plan = planMirrorFlow(in);
    CHECK(plan.missing);
    CHECK_FALSE(plan.pending);
    CHECK(plan.remove);
    CHECK(unofferedMirrorReason(true, true) == "origin_missing");
    CHECK(unofferedMirrorReason(false, false) == "peer_down");
    CHECK(unofferedMirrorReason(false, true) == "peer_down");
}

TEST_CASE("peer inventory: settled after one full poll of the same process with a scanned inventory")
{
    PeerSnapshot peer;
    peer.host_id = "node-b";
    peer.boot_id = "boot-1";
    peer.inventory.revision = 3;
    peer.up = false;
    CHECK(nextPollCount(nullptr, peer) == 0);
    peer.up = true;
    peer.polls = nextPollCount(nullptr, peer); // the first poll after this agent started
    CHECK(peer.polls == 1);
    CHECK_FALSE(inventorySettled(peer));
    auto const first = peer;
    peer.polls = nextPollCount(&first, peer); // one full poll later
    CHECK(peer.polls == 2);
    CHECK(inventorySettled(peer));
    auto const second = peer;
    peer.polls = nextPollCount(&second, peer);
    CHECK(peer.polls == 3);
    CHECK(inventorySettled(peer));

    // The peer restarted (another boot id): its first inventory may not list its flows yet.
    auto const before = peer;
    peer.boot_id = "boot-2";
    peer.polls = nextPollCount(&before, peer);
    CHECK(peer.polls == 1);
    CHECK_FALSE(inventorySettled(peer));

    // Down in between: counting starts again.
    auto down = peer;
    down.up = false;
    down.polls = nextPollCount(&peer, down);
    CHECK(down.polls == 0);
    CHECK_FALSE(inventorySettled(down));
    auto back = down;
    back.up = true;
    back.polls = nextPollCount(&down, back);
    CHECK(back.polls == 1);

    // An agent before its first scan answers revision 0 and no domain (before 1.7.1; 503 since).
    PeerSnapshot fresh;
    fresh.up = true;
    fresh.polls = 5;
    fresh.inventory.revision = 0;
    CHECK_FALSE(inventorySettled(fresh));
    fresh.inventory.revision = 1;
    CHECK(inventorySettled(fresh));
}

TEST_CASE("reader scan per flow: a reader of one flow keeps only that flow")
{
    auto const root = tempDir("mfa-flow-readers");
    auto const mirror = root / "mxl" / "mirror-d1";
    auto const busy = mirror / "f1.mxl-flow";
    auto const stale = mirror / "f2.mxl-flow";
    write(busy / "data", "x");
    write(busy / "grains" / "0", "g");
    write(stale / "data", "y");
    write(stale / "grains" / "0", "h");
    write(mirror / "domain_def.json", "{}");
    std::map<FileKey, std::string> files;
    collectFiles(busy.string(), busy.string(), files);
    collectFiles(stale.string(), stale.string(), files);
    CHECK(files.size() == 4);

    auto const proc = root / "proc";
    write(proc / "self" / "status", "NSpid:\t100\n");
    write(proc / "100" / "maps", mapsLine(stale / "data")); // the agent's own writer
    write(proc / "300" / "maps", mapsLine(busy / "grains" / "0"));
    write(proc / "300" / "comm", "player\n");
    auto const scan = scanReaders(files, proc.string(), 100);
    CHECK(scan.complete);
    std::vector<ReaderProcess> readers;
    CHECK(removalBlocker(scan, busy.string(), &readers) == "mapped by 300(player)");
    readers.clear();
    CHECK(removalBlocker(scan, stale.string(), &readers).empty());
    CHECK(readers.empty());
    fs::remove_all(root);
}

TEST_CASE("mirror cleanup settings")
{
    auto cfg = parseConfig({});
    CHECK(cfg.mirror_idle_grace_s == 300);
    CHECK_FALSE(cfg.mirror_cleanup_dry_run);
    cfg = parseConfig({{"MIRROR_IDLE_GRACE_S", "0"}, {"MIRROR_CLEANUP_DRY_RUN", "true"}});
    CHECK(cfg.mirror_idle_grace_s == 0);
    CHECK(cfg.mirror_cleanup_dry_run);
    CHECK_THROWS_AS(parseConfig({{"MIRROR_IDLE_GRACE_S", "-1"}}), ConfigError);
}

TEST_CASE("mirror rebuild hook setting: environment only")
{
    // Only the deployment opens the hook: the environment sets it, the file layer cannot.
    CHECK_FALSE(loadLayered({}, {}).mirror_admin_rebuild);
    CHECK(loadLayered({}, {{"MIRROR_ADMIN_REBUILD", "true"}}).mirror_admin_rebuild);
    CHECK_FALSE(loadLayered({{"MIRROR_ADMIN_REBUILD", "true"}}, {}).mirror_admin_rebuild);
    CHECK_THROWS_AS(loadLayered({}, {{"MIRROR_ADMIN_REBUILD", "maybe"}}), ConfigError);
    CHECK_FALSE(parseConfig({{"MIRROR_ADMIN_REBUILD", "true"}}).mirror_admin_rebuild);
    CHECK(isEnvironmentOnlyKey("MIRROR_ADMIN_REBUILD"));
    auto const keys = configKeys();
    CHECK(std::find(keys.begin(), keys.end(), "MIRROR_ADMIN_REBUILD") == keys.end());
    CHECK(configToMap(loadLayered({}, {{"MIRROR_ADMIN_REBUILD", "true"}})).count("MIRROR_ADMIN_REBUILD") == 0);
    char const* envp[] = {"MIRROR_ADMIN_REBUILD=true", "UNRELATED=1", nullptr};
    CHECK(environmentValues(envp).at("MIRROR_ADMIN_REBUILD") == "true");

    // PUT /api/v1/config and POST /api/v1/config/import cannot set it; the start value stays.
    std::map<std::string, ValueOrigin> origin;
    auto cfg = loadLayered({}, {{"MIRROR_ADMIN_REBUILD", "true"}}, &origin);
    cfg.config_file.clear();
    ConfigStore store(cfg, origin, {});
    CHECK_THROWS_AS(store.updateFile({{"MIRROR_ADMIN_REBUILD", "false"}}), ConfigError);
    store.importDocument({{"MIRROR_ADMIN_REBUILD", "false"}, {"MIRROR_STALE_MS", "1500"}});
    CHECK(store.get().mirror_admin_rebuild);
    CHECK(store.get().mirror_stale_ms == 1500);
    CHECK(store.exportJson().find("MIRROR_ADMIN_REBUILD") == std::string::npos);
    store.updateFile({{"MIRROR_STALE_MS", "1000"}});
    CHECK(store.get().mirror_admin_rebuild);

    ConfigStore off(loadLayered({}, {}, &origin), origin, {});
    off.importDocument({{"MIRROR_ADMIN_REBUILD", "true"}});
    CHECK_FALSE(off.get().mirror_admin_rebuild);
}

TEST_CASE("maps line: device and inode")
{
    auto const key = parseMapsLine("7f1c2a000000-7f1c2a400000 rw-s 00000000 00:2a 123456    /Volumes/mxl/mirror-x/f.mxl-flow/data");
    REQUIRE(key.has_value());
    CHECK(std::get<0>(*key) == 0);
    CHECK(std::get<1>(*key) == 0x2a);
    CHECK(std::get<2>(*key) == 123456);
    CHECK_FALSE(parseMapsLine("7f1c2a000000-7f1c2a400000 rw-p 00000000 00:00 0 ").has_value());
    CHECK_FALSE(parseMapsLine("garbage").has_value());
}

TEST_CASE("nested PID namespace")
{
    CHECK_FALSE(nestedPidNamespace("Name:\tagent\nNSpid:\t4242\n"));
    CHECK(nestedPidNamespace("Name:\tagent\nNSpid:\t4242\t7\n"));
    CHECK_FALSE(nestedPidNamespace("Name:\tagent\n"));
}

TEST_CASE("reader scan: finds other processes, skips the agent")
{
    auto const root = tempDir("mfa-readers");
    auto const mirror = root / "mxl" / "mirror-d1";
    auto const data = mirror / "f1.mxl-flow" / "data";
    write(data, "x");
    write(mirror / "domain_def.json", "{}");
    std::map<FileKey, std::string> files;
    collectFiles(mirror.string(), mirror.string(), files);
    CHECK(files.size() == 2);

    auto const proc = root / "proc";
    write(proc / "self" / "status", "NSpid:\t100\n");
    write(proc / "100" / "maps", mapsLine(data)); // the agent itself
    write(proc / "100" / "comm", "mxl-fabrics-agent\n");
    auto scan = scanReaders(files, proc.string(), 100);
    CHECK(scan.complete);
    CHECK(scan.readers.empty());

    write(proc / "200" / "maps", "7f0000000000-7f0000001000 r-xp 00000000 08:01 99 /usr/lib/libc.so\n" + mapsLine(data));
    write(proc / "200" / "comm", "vmix\n");
    scan = scanReaders(files, proc.string(), 100);
    REQUIRE(scan.readers.count(mirror.string()) == 1);
    CHECK(scan.readers[mirror.string()].size() == 1);
    CHECK(scan.readers[mirror.string()][0].pid == 200);
    CHECK(scan.readers[mirror.string()][0].comm == "vmix");

    write(proc / "self" / "status", "NSpid:\t100\t1\n");
    scan = scanReaders(files, proc.string(), 100);
    CHECK_FALSE(scan.complete);
    fs::remove_all(root);
}

namespace
{
PeerSnapshot peerAt(std::string const& host, bool up, std::uint32_t polls, std::uint64_t revision = 3)
{
    PeerSnapshot peer;
    peer.host_id = host;
    peer.up = up;
    peer.polls = polls;
    peer.inventory.revision = revision;
    return peer;
}
} // namespace

TEST_CASE("settle tracker: per peer, pending only while it settles, a flapping peer counts as down after the limit")
{
    using namespace std::chrono_literals;
    auto const t0 = std::chrono::steady_clock::time_point{} + 1h;
    auto const limit = 60s;
    InventorySettleTracker tracker;

    // node-b settled, node-c just restarted (first poll of its new process).
    tracker.update({peerAt("node-b", true, 5), peerAt("node-c", true, 1)}, t0, limit);
    CHECK(tracker.view("node-b") == InventoryView::Settled);
    CHECK(tracker.view("node-c") == InventoryView::Pending);
    CHECK(tracker.view("node-x") == InventoryView::Down); // unknown
    CHECK(tracker.anyPending());
    // A domain from node-b that only node-b lists is decided; one that node-c lists too waits.
    CHECK(tracker.settledFor("node-b", {"node-b"}));
    CHECK(tracker.settledFor("node-b", {}));
    CHECK_FALSE(tracker.settledFor("node-b", {"node-b", "node-c"}));
    CHECK_FALSE(tracker.settledFor("node-c", {}));

    // One poll later node-c is settled.
    tracker.update({peerAt("node-b", true, 6), peerAt("node-c", true, 2)}, t0 + 2s, limit);
    CHECK(tracker.view("node-c") == InventoryView::Settled);
    CHECK_FALSE(tracker.anyPending());

    // node-c flaps: down every other poll, or a new boot id every poll (polls stays 1). Passes while it is
    // down do not reset the wait; after the limit it counts as down and holds nothing back.
    auto t = t0 + 4s;
    for (int i = 0; i < 40; ++i, t += 2s)
    {
        bool const up = i % 2 == 0;
        tracker.update({peerAt("node-b", true, 7), peerAt("node-c", up, up ? 1 : 0)}, t, limit);
        if (up)
        {
            CHECK(tracker.view("node-c") == (t - (t0 + 4s) < limit ? InventoryView::Pending : InventoryView::Down));
        }
        else
        {
            CHECK(tracker.view("node-c") == InventoryView::Down);
        }
    }
    CHECK(tracker.view("node-c") != InventoryView::Pending);
    CHECK(tracker.settledFor("node-b", {"node-b", "node-c"}));
    CHECK_FALSE(tracker.anyPending());

    // Once it settles, the wait starts from scratch the next time.
    tracker.update({peerAt("node-b", true, 8), peerAt("node-c", true, 2)}, t, limit);
    CHECK(tracker.view("node-c") == InventoryView::Settled);
    tracker.update({peerAt("node-b", true, 9), peerAt("node-c", true, 1)}, t + 2s, limit);
    CHECK(tracker.view("node-c") == InventoryView::Pending);

    // Pending for one poll, then down for longer than the limit: it comes back with the full wait, not as down.
    auto const back = t + 4s + limit + 10s;
    tracker.update({peerAt("node-b", true, 9), peerAt("node-c", false, 0)}, t + 4s, limit);
    tracker.update({peerAt("node-b", true, 9), peerAt("node-c", false, 0)}, back - 1s, limit);
    tracker.update({peerAt("node-b", true, 9), peerAt("node-c", true, 1)}, back, limit);
    CHECK(tracker.view("node-c") == InventoryView::Pending);

    // An inventory before the first scan is never settled.
    tracker.update({peerAt("node-d", true, 4, 0)}, back + 2s, limit);
    CHECK(tracker.view("node-d") == InventoryView::Pending);
}

TEST_CASE("missing clock: pauses while pending, keeps its start, does not count the paused time")
{
    using namespace std::chrono_literals;
    auto const t0 = std::chrono::steady_clock::time_point{} + 1h;
    MissingClock clock{t0, std::nullopt};
    CHECK(stepMissingClock(clock, false, t0 + 100s) == 100s);
    // An unrelated peer or the source restarts: pending for 10 s. The clock stays at 100 s.
    CHECK(stepMissingClock(clock, true, t0 + 101s) == 101s);
    CHECK(stepMissingClock(clock, true, t0 + 111s) == 101s);
    // Settled again: it goes on from where it stopped.
    CHECK(stepMissingClock(clock, false, t0 + 111s) == 101s);
    CHECK(stepMissingClock(clock, false, t0 + 310s) == 300s);
}

