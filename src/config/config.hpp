#pragma once

#include "util/cidr.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace mfa
{
struct ConfigError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct PeerConfig
{
    std::string host_id;
    std::string control_url;
    std::string local_fabric_addr;
    std::string remote_fabric_addr;
    std::string provider;
};

struct Config
{
    std::string host_id;
    std::string mxl_root = "/Volumes/mxl";
    std::string state_dir = "/config";
    int scan_interval_ms = 2000;
    std::string mirror_mode = "eager";
    std::vector<std::string> mirror_include_domains;
    std::vector<std::string> mirror_include_flows;
    std::vector<std::string> mirror_exclude_domains;
    std::vector<std::string> mirror_exclude_flows;
    int mirror_grace_s = 10;
    int tmpfs_reserve_mb = 512;
    bool cleanup_mirrors_on_exit = false;
    std::string default_provider = "verbs";
    std::string provider_fallback;
    std::string fabric_interface;
    int fabric_port_base = 23500;
    int fabric_port_count = 100;
    std::vector<PeerConfig> peers;
    int peer_poll_interval_ms = 2000;
    int release_grace_ms = 2000;
    std::string transfer_pacing = "off"; // off | frame: send a grain in slice batches spread over part of its duration
    double transfer_pacing_spread = 0.5; // share of the grain duration the batches start within
    int transfer_pacing_batches = 8;
    bool nmos_enable = true;
    std::string nmos_seed;
    std::string nmos_label;
    std::map<std::string, std::vector<std::string>> nmos_tags;
    bool nmos_dns_sd = false;
    std::string nmos_host_address;
    std::string nmos_registry_address;
    int nmos_registry_port = 3210;
    std::string nmos_query_address;
    int nmos_query_port = 3211;
    int nmos_poll_interval_ms = 1000;
    int nmos_port = 3232;
    int shutdown_timeout_s = 10;
    std::vector<std::string> local_node_ids;
    std::vector<std::string> local_node_hostnames;
    std::vector<IpCidr> local_node_cidrs;
    int web_port = 8095;
    bool web_enable = true;
    int rt_priority = 0;
    std::vector<int> cpu_affinity;
    std::string log_level = "info";
    bool metrics_per_flow = true;
    std::string config_file;

    std::string queryHost() const
    {
        return nmos_query_address.empty() ? nmos_registry_address : nmos_query_address;
    }

    // A registry is in use when an address is set, or DNS-SD browsing is explicitly on.
    bool registryConfigured() const
    {
        return nmos_dns_sd || !nmos_registry_address.empty();
    }
};

enum class ValueOrigin
{
    Default,
    File,
    Env,
};

struct ConfigLayer
{
    std::map<std::string, std::string> values;
};

// Flat env-style map. PEERS is a JSON array string. Lists are comma-separated.
Config parseConfig(std::map<std::string, std::string> const& values);
Config loadLayered(std::map<std::string, std::string> const& fileValues, std::map<std::string, std::string> const& envValues,
    std::map<std::string, ValueOrigin>* origin = nullptr);

bool isRuntimeKey(std::string const& key);
std::vector<std::string> configKeys();
std::string configToJson(Config const& cfg);
std::string configToEnv(Config const& cfg);
std::map<std::string, std::string> configToMap(Config const& cfg);

std::map<std::string, std::string> environmentValues(char const* const* envp);
std::map<std::string, std::string> loadConfigFile(std::string const& path);
} // namespace mfa
