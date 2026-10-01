#include <doctest/doctest.h>

#include "inventory/model.hpp"
#include "inventory/scanner.hpp"

#include <filesystem>
#include <fstream>

using namespace mfa;

TEST_CASE("mirror marker classification")
{
    auto const own = R"({"id":"dom","x-mxl-fabrics-agent":{"mirror":true,"source_host_id":"a","owner_host_id":"me"}})";
    auto const foreign = R"({"id":"dom","x-mxl-fabrics-agent":{"mirror":true,"source_host_id":"a","owner_host_id":"other"}})";
    auto const local = R"({"id":"dom","label":"decklink"})";
    CHECK(classifyDomain(own, "me", false) == "mirror");
    CHECK(classifyDomain(foreign, "me", false) == "conflict");
    CHECK(classifyDomain(local, "me", false) == "local");
    CHECK(classifyDomain(local, "me", true) == "conflict");
}

TEST_CASE("flow record from a descriptor")
{
    auto const def = R"({"id":"11111111-1111-1111-1111-111111111111","format":"urn:x-nmos:format:video","media_type":"video/v210","frame_width":1920,"frame_height":1080,"grain_rate":{"numerator":50,"denominator":1}})";
    auto const options = R"({"urn:x-mxl:option:history_duration/v1.0":200000000})";
    auto flow = flowFromDef(def, options, true);
    CHECK(flow.format == "discrete");
    CHECK(flow.flow_id == "11111111-1111-1111-1111-111111111111");
    CHECK(flow.active);
    CHECK(flow.ring_depth == 10);
    CHECK(flow.payload_size > 0);
}

TEST_CASE("scanner classifies directories")
{
    auto const root = std::filesystem::temp_directory_path() / "mfa-scan-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "local" );
    std::filesystem::create_directories(root / "mirror-dom");
    {
        std::ofstream(root / "local" / "domain_def.json") << R"({"id":"dom-local"})";
        std::ofstream(root / "mirror-dom" / "domain_def.json")
            << R"({"id":"dom-remote","x-mxl-fabrics-agent":{"mirror":true,"source_host_id":"a","owner_host_id":"me"}})";
    }
    auto inventory = scanOnce(root.string(), "me", {});
    int local = 0;
    int mirror = 0;
    for (auto const& domain : inventory.domains)
    {
        if (domain.kind == "local")
        {
            ++local;
        }
        if (domain.kind == "mirror")
        {
            ++mirror;
        }
    }
    CHECK(local == 1);
    CHECK(mirror == 1);
    CHECK(inventory.canonicalJson().find("dom-remote") == std::string::npos);
    std::filesystem::remove_all(root);
}
