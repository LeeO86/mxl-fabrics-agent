#include <doctest/doctest.h>

#include "config/config.hpp"
#include "config/store.hpp"
#include "util/uuid.hpp"

using namespace mfa;

std::map<std::string, std::string> baseValues()
{
    return {{"HOST_ID", "h"}, {"NMOS_HOST_ADDRESS", "192.0.2.10"}};
}

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
    CHECK_FALSE(cfg.nmos_dns_sd);
    CHECK(cfg.nmos_query_port == cfg.nmos_registry_port + 1);
    CHECK(cfg.shutdown_timeout_s == 10);
    CHECK(cfg.state_dir == "/config");
    CHECK(cfg.cleanup_mirrors_on_exit == false);
    CHECK_FALSE(cfg.nmos_host_address.empty());
    CHECK(cfg.nmos_host_address != "127.0.0.1");
    CHECK(cfg.nmos_label == cfg.host_id);
    CHECK(nmosNodeId(cfg.nmos_seed, cfg.host_id) == nodeIdForHost(cfg.host_id).str());

    std::map<std::string, std::string> file = {{"WEB_PORT", "9000"}, {"MIRROR_MODE", "on-demand"}, {"HOST_ID", "file-host"},
        {"NMOS_HOST_ADDRESS", "192.0.2.10"}};
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
    auto values = baseValues();
    values["MIRROR_MODE"] = "sometimes";
    CHECK_THROWS_AS(parseConfig(values), ConfigError);
    values = baseValues();
    values["MIRROR_INCLUDE"] = "abc";
    CHECK_THROWS_AS(parseConfig(values), ConfigError);
}

TEST_CASE("platform setting aliases and defaults")
{
    auto rootOnly = baseValues();
    rootOnly["MXL_ROOT"] = "/data/mxl";
    CHECK(parseConfig(rootOnly).mxl_root == "/data/mxl");
    auto scanOnly = baseValues();
    scanOnly["MXL_DOMAIN_SCAN_PATH"] = "/data/scan";
    CHECK(parseConfig(scanOnly).mxl_root == "/data/scan");
    auto both = baseValues();
    both["MXL_ROOT"] = "/old";
    both["MXL_DOMAIN_SCAN_PATH"] = "/new";
    CHECK_THROWS_AS(parseConfig(both), ConfigError);

    std::map<std::string, ValueOrigin> origin;
    auto cfg = loadLayered({{"MXL_DOMAIN_SCAN_PATH", "/from-file"}, {"HOST_ID", "h"}, {"NMOS_HOST_ADDRESS", "192.0.2.10"}},
        {{"MXL_ROOT", "/from-env"}}, &origin);
    CHECK(cfg.mxl_root == "/from-env");
    CHECK(origin["MXL_ROOT"] == ValueOrigin::Env);
    CHECK(origin["MXL_DOMAIN_SCAN_PATH"] == ValueOrigin::Env);

    cfg = loadLayered({{"MXL_ROOT", "/from-file"}, {"HOST_ID", "h"}, {"NMOS_HOST_ADDRESS", "192.0.2.10"}},
        {{"MXL_DOMAIN_SCAN_PATH", "/from-env"}}, &origin);
    CHECK(cfg.mxl_root == "/from-env");

    auto cleanup = baseValues();
    cleanup["CLEANUP_MIRRORS_ON_EXIT"] = "true";
    CHECK(parseConfig(cleanup).cleanup_mirrors_on_exit);
    cleanup = baseValues();
    cleanup["MXL_CLEANUP_ON_EXIT"] = "true";
    CHECK(parseConfig(cleanup).cleanup_mirrors_on_exit);
    cleanup["CLEANUP_MIRRORS_ON_EXIT"] = "false";
    CHECK_THROWS_AS(parseConfig(cleanup), ConfigError);

    auto ports = baseValues();
    ports["NMOS_REGISTRY_PORT"] = "4000";
    CHECK(parseConfig(ports).nmos_query_port == 4001);
    ports["NMOS_QUERY_PORT"] = "3211";
    CHECK(parseConfig(ports).nmos_query_port == 3211);

    auto seeded = baseValues();
    seeded["NMOS_SEED"] = "sport-fabrics";
    seeded["NMOS_LABEL"] = "fabrics-a";
    seeded["NMOS_TAGS"] = R"({"urn:x-srf:production":["sport-sa"],"urn:x-srf:function":["fabrics"]})";
    cfg = parseConfig(seeded);
    CHECK(cfg.nmos_label == "fabrics-a");
    CHECK(cfg.nmos_tags["urn:x-srf:production"].at(0) == "sport-sa");
    CHECK(nmosNodeId(cfg.nmos_seed, cfg.host_id) == nodeIdForHost("sport-fabrics").str());
    CHECK(nmosNodeId("", cfg.host_id) == nodeIdForHost(cfg.host_id).str());
    auto exported = configToJson(cfg);
    CHECK(exported.find("urn:x-srf:function") != std::string::npos);
    CHECK(exported.find("MXL_DOMAIN_SCAN_PATH") != std::string::npos);
    CHECK(exported.find("MXL_ROOT") != std::string::npos);

    auto badHost = baseValues();
    badHost["NMOS_HOST_ADDRESS"] = "node-a";
    CHECK_THROWS_AS(parseConfig(badHost), ConfigError);
    badHost["NMOS_HOST_ADDRESS"] = "127.0.0.1";
    CHECK_THROWS_AS(parseConfig(badHost), ConfigError);
    badHost["NMOS_HOST_ADDRESS"] = "0.0.0.0";
    CHECK_THROWS_AS(parseConfig(badHost), ConfigError);
    badHost["NMOS_HOST_ADDRESS"] = "::1";
    CHECK_THROWS_AS(parseConfig(badHost), ConfigError);
    badHost["NMOS_TAGS"] = "[]";
    badHost["NMOS_HOST_ADDRESS"] = "192.0.2.10";
    CHECK_THROWS_AS(parseConfig(badHost), ConfigError);
    CHECK(parseConfig(baseValues()).nmos_dns_sd == false);
    auto dns = baseValues();
    dns["NMOS_DNS_SD"] = "true";
    CHECK(parseConfig(dns).nmos_dns_sd);

    ConfigStore store(parseConfig(baseValues()), {}, {});
    store.importDocument({{"MIRROR_MODE", "on-demand"}, {"NOT_A_SETTING", "x"}, {"HOST_ID", "other"}});
    CHECK(store.get().mirror_mode == "on-demand");
    CHECK(store.get().host_id == "other");
    auto again = configToJson(store.get());
    CHECK(again.find("on-demand") != std::string::npos);
}

TEST_CASE("peers json")
{
    auto cfg = parseConfig({{"HOST_ID", "h"},
        {"PEERS", R"([{"host_id":"node-b","local_fabric_addr":"192.168.12.1","remote_fabric_addr":"192.168.12.2","provider":"verbs"}])"}});
    REQUIRE(cfg.peers.size() == 1);
    CHECK(cfg.peers[0].host_id == "node-b");
    CHECK(cfg.peers[0].local_fabric_addr == "192.168.12.1");
}
