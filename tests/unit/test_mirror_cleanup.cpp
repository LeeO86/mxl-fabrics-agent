#include <doctest/doctest.h>

#include "config/config.hpp"
#include "mirror/lifecycle.hpp"
#include "mirror/readers.hpp"

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

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
    in.grace_expired = true;
    auto const plan = planMirrorFlow(in);
    CHECK(plan.missing); // the grace time runs meanwhile
    CHECK_FALSE(plan.remove);
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
