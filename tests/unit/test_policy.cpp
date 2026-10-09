#include <doctest/doctest.h>

#include "inventory/model.hpp"
#include "replication/policy.hpp"
#include "util/net.hpp"

#include <filesystem>
#include <fstream>

using namespace mfa;
using namespace std::chrono_literals;

namespace
{
void writeFile(std::filesystem::path const& path, std::string const& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << text;
}
} // namespace

TEST_CASE("retry backoff doubles from 250 ms up to 30 s")
{
    CHECK(nextBackoff(0ms) == 250ms);
    CHECK(nextBackoff(250ms) == 500ms);
    CHECK(nextBackoff(20000ms) == 30000ms);
    CHECK(nextBackoff(30000ms) == 30000ms);
}

TEST_CASE("a destination rebuilds only when grains are due and none came")
{
    StallInput in;
    in.sinceGrain = 60s;
    CHECK_FALSE(shouldRebuild(in)); // nothing says grains are due: an idle origin

    in.originMoved = true;
    in.sinceGrain = 4s;
    CHECK_FALSE(shouldRebuild(in));
    in.sinceGrain = 6s;
    CHECK(shouldRebuild(in));
    in.stalls = 1;
    CHECK_FALSE(shouldRebuild(in)); // 10 s after one rebuild in a row
    in.stalls = 9;
    in.sinceGrain = 39s;
    CHECK_FALSE(shouldRebuild(in)); // at most 40 s
    in.sinceGrain = 41s;
    CHECK(shouldRebuild(in));

    StallInput source;
    source.sourceError = true; // the source reports a transfer that does not complete
    source.sinceGrain = 500ms;
    CHECK_FALSE(shouldRebuild(source));
    source.sinceGrain = 1500ms;
    CHECK(shouldRebuild(source));

    StallInput dest;
    dest.destError = true;
    dest.sinceGrain = 5500ms;
    CHECK(shouldRebuild(dest));
}

TEST_CASE("a re-created or deleted origin flow is seen by its inode")
{
    auto const dir = std::filesystem::temp_directory_path() / "mfa-origin-test";
    std::filesystem::remove_all(dir);
    auto const data = dir / "data";
    writeFile(data, "first");
    auto const inode = fileInode(data.string());
    CHECK(inode != 0);
    CHECK(originChange(data.string(), inode).empty());

    // A new file in its place (created while the old one exists, so the inode differs).
    writeFile(dir / "next", "second");
    std::filesystem::rename(dir / "next", data);
    CHECK(originChange(data.string(), inode) == "origin flow re-created");

    std::filesystem::remove(data);
    CHECK(originChange(data.string(), inode) == "origin flow deleted");
    CHECK(fileInode(data.string()) == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("the origin that is being written wins when several hosts hold a flow")
{
    std::vector<OriginCandidate> stale{{"mxl-host-01", true, false}, {"mxl-host-03", true, true}};
    CHECK(chooseOrigin(stale, "") == "mxl-host-03");
    CHECK(chooseOrigin(stale, "mxl-host-01") == "mxl-host-03");

    std::vector<OriginCandidate> writer{{"b", false, false}, {"c", true, false}};
    CHECK(chooseOrigin(writer, "b") == "c");

    std::vector<OriginCandidate> equal{{"c", true, true}, {"b", true, true}};
    CHECK(chooseOrigin(equal, "") == "b");  // lowest host id
    CHECK(chooseOrigin(equal, "c") == "c"); // the current choice stays
    CHECK(chooseOrigin({}, "x").empty());
}

TEST_CASE("a flow is live when its head is at the current time")
{
    double const tai = 1.8e9;
    CHECK(headIsLive(static_cast<std::uint64_t>(tai * 50), 50, 1, tai));
    CHECK(headIsLive(static_cast<std::uint64_t>(tai * 50) - 100, 50, 1, tai)); // 2 s behind
    CHECK_FALSE(headIsLive(static_cast<std::uint64_t>(tai * 50) - 86400 * 50, 50, 1, tai)); // a day old
    CHECK(headIsLive(static_cast<std::uint64_t>(tai * 48000), 48000, 1, tai));             // audio, samples
    CHECK_FALSE(headIsLive(0, 50, 1, tai));
    CHECK_FALSE(headIsLive(UINT64_MAX, 50, 1, tai));
}

TEST_CASE("fabric link state from carrier and RDMA counters")
{
    auto const root = std::filesystem::temp_directory_path() / "mfa-link-test";
    std::filesystem::remove_all(root);
    auto const net = (root / "net").string();
    auto const ib = (root / "ib").string();

    CHECK(fabricLink("", net, ib).up); // no address: nothing to check

    writeFile(root / "net" / "eth9" / "carrier", "1\n");
    auto up = fabricLink("eth9", net, ib);
    CHECK(up.up);
    CHECK(up.netdev == "eth9");
    CHECK(up.rdmaDevice.empty());
    CHECK_FALSE(up.retransmits);

    std::filesystem::create_directories(root / "net" / "eth9" / "device" / "infiniband" / "rocep9s0f1");
    writeFile(root / "ib" / "rocep9s0f1" / "ports" / "1" / "hw_counters" / "RetransSegs", "4414632\n");
    writeFile(root / "net" / "eth9" / "carrier", "0\n");
    writeFile(root / "net" / "eth9" / "operstate", "down\n");
    auto down = fabricLink("eth9", net, ib);
    CHECK_FALSE(down.up);
    CHECK(down.error == "no carrier on eth9 (down)");
    CHECK(down.rdmaDevice == "rocep9s0f1");
    REQUIRE(down.retransmits);
    CHECK(*down.retransmits == 4414632);

    // An address no local interface holds (irdma: "Failed to get source interface information").
    auto gone = fabricLink("198.51.100.77", net, ib);
    CHECK_FALSE(gone.up);
    CHECK(gone.error == "source interface unavailable: no local interface has 198.51.100.77");
    std::filesystem::remove_all(root);
}
