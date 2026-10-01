#include <doctest/doctest.h>

#include "config/config.hpp"

using namespace mfa;

TEST_CASE("config defaults and precedence")
{
    setenv("HOSTNAME", "agent-host", 1);
    auto cfg = parseConfig({});
    CHECK(cfg.host_id == "agent-host");
    CHECK(cfg.mxl_root == "/Volumes/mxl");
    CHECK(cfg.mirror_mode == "eager");
    CHECK(cfg.web_port == 8095);
    CHECK(cfg.nmos_port == 3232);
    CHECK(cfg.metrics_per_flow);

    std::map<std::string, std::string> file = {{"WEB_PORT", "9000"}, {"MIRROR_MODE", "on-demand"}, {"HOST_ID", "file-host"}};
    std::map<std::string, std::string> env = {{"WEB_PORT", "9001"}, {"HOST_ID", "env-host"}};
    std::map<std::string, ValueOrigin> origin;
    cfg = loadLayered(file, env, &origin);
    CHECK(cfg.web_port == 9001);
    CHECK(cfg.host_id == "env-host");
    CHECK(cfg.mirror_mode == "on-demand");
    CHECK(origin["WEB_PORT"] == ValueOrigin::Env);
    CHECK(origin["MIRROR_MODE"] == ValueOrigin::File);
    CHECK(isRuntimeKey("PEERS"));
    CHECK_FALSE(isRuntimeKey("WEB_PORT"));
}

TEST_CASE("config rejects combined include keys and bad mode")
{
    CHECK_THROWS_AS(parseConfig({{"HOST_ID", "h"}, {"MIRROR_MODE", "sometimes"}}), ConfigError);
    CHECK_THROWS_AS(parseConfig({{"HOST_ID", "h"}, {"MIRROR_INCLUDE", "abc"}}), ConfigError);
}

TEST_CASE("peers json")
{
    auto cfg = parseConfig({{"HOST_ID", "h"},
        {"PEERS", R"([{"host_id":"node-b","local_fabric_addr":"192.168.12.1","remote_fabric_addr":"192.168.12.2","provider":"verbs"}])"}});
    REQUIRE(cfg.peers.size() == 1);
    CHECK(cfg.peers[0].host_id == "node-b");
    CHECK(cfg.peers[0].local_fabric_addr == "192.168.12.1");
}
