#include "config/config.hpp"

#include "util/jsonutil.hpp"

#include <cstdlib>
#include <set>
#include <sstream>

namespace mfa
{
namespace
{
std::vector<std::string> splitComma(std::string const& text)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : text)
    {
        if (c == ',')
        {
            if (!cur.empty())
            {
                out.push_back(cur);
            }
            cur.clear();
        }
        else if (c != ' ' && c != '\t' && c != '\n')
        {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
    {
        out.push_back(cur);
    }
    return out;
}

std::string joinComma(std::vector<std::string> const& items)
{
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        if (i != 0)
        {
            out += ",";
        }
        out += items[i];
    }
    return out;
}

int parseInt(std::string const& key, std::string const& text, int min, int max)
{
    try
    {
        std::size_t used = 0;
        int const value = std::stoi(text, &used);
        if (used != text.size() || value < min || value > max)
        {
            throw ConfigError(key + " is out of range");
        }
        return value;
    }
    catch (ConfigError const&)
    {
        throw;
    }
    catch (...)
    {
        throw ConfigError(key + " is not an integer");
    }
}

bool parseBool(std::string const& key, std::string const& text)
{
    if (text == "1" || text == "true" || text == "TRUE" || text == "yes")
    {
        return true;
    }
    if (text == "0" || text == "false" || text == "FALSE" || text == "no" || text.empty())
    {
        return false;
    }
    throw ConfigError(key + " must be true or false");
}

std::vector<int> parseIntList(std::string const& key, std::string const& text)
{
    std::vector<int> out;
    if (text.empty())
    {
        return out;
    }
    for (auto const& part : splitComma(text))
    {
        out.push_back(parseInt(key, part, 0, 1024));
    }
    return out;
}

std::vector<PeerConfig> parsePeers(std::string const& text)
{
    if (text.empty() || text == "[]")
    {
        return {};
    }
    std::string err;
    auto const root = json::parse(text, &err);
    if (!err.empty() || !root.is<picojson::array>())
    {
        throw ConfigError("PEERS must be a JSON array");
    }
    std::vector<PeerConfig> peers;
    std::set<std::string> seen;
    for (auto const& item : root.get<picojson::array>())
    {
        if (!item.is<picojson::object>())
        {
            throw ConfigError("PEERS entries must be objects");
        }
        auto const& obj = item.get<picojson::object>();
        PeerConfig peer;
        peer.host_id = json::asString(obj, "host_id").value_or("");
        if (peer.host_id.empty())
        {
            throw ConfigError("PEERS entry is missing host_id");
        }
        if (!seen.insert(peer.host_id).second)
        {
            throw ConfigError("duplicate PEERS host_id " + peer.host_id);
        }
        peer.control_url = json::asString(obj, "control_url").value_or("");
        peer.local_fabric_addr = json::asString(obj, "local_fabric_addr").value_or("");
        peer.remote_fabric_addr = json::asString(obj, "remote_fabric_addr").value_or("");
        peer.provider = json::asString(obj, "provider").value_or("");
        if (!peer.provider.empty() && peer.provider != "verbs" && peer.provider != "tcp")
        {
            throw ConfigError("PEERS provider must be verbs or tcp");
        }
        peers.push_back(std::move(peer));
    }
    return peers;
}

std::string peersToJson(std::vector<PeerConfig> const& peers)
{
    picojson::array arr;
    for (auto const& peer : peers)
    {
        picojson::object obj;
        obj["host_id"] = picojson::value(peer.host_id);
        if (!peer.control_url.empty())
        {
            obj["control_url"] = picojson::value(peer.control_url);
        }
        if (!peer.local_fabric_addr.empty())
        {
            obj["local_fabric_addr"] = picojson::value(peer.local_fabric_addr);
        }
        if (!peer.remote_fabric_addr.empty())
        {
            obj["remote_fabric_addr"] = picojson::value(peer.remote_fabric_addr);
        }
        if (!peer.provider.empty())
        {
            obj["provider"] = picojson::value(peer.provider);
        }
        arr.emplace_back(obj);
    }
    return picojson::value(arr).serialize();
}

std::string valueOr(std::map<std::string, std::string> const& values, char const* key, char const* fallback)
{
    auto const it = values.find(key);
    if (it == values.end())
    {
        return fallback;
    }
    return it->second;
}
} // namespace

std::vector<std::string> configKeys()
{
    return {"HOST_ID", "MXL_ROOT", "SCAN_INTERVAL_MS", "MIRROR_MODE", "MIRROR_INCLUDE_DOMAINS", "MIRROR_INCLUDE_FLOWS",
        "MIRROR_EXCLUDE_DOMAINS", "MIRROR_EXCLUDE_FLOWS", "MIRROR_GRACE_S", "TMPFS_RESERVE_MB", "CLEANUP_MIRRORS_ON_EXIT",
        "DEFAULT_PROVIDER", "PROVIDER_FALLBACK", "FABRIC_INTERFACE", "FABRIC_PORT_BASE", "FABRIC_PORT_COUNT", "PEERS",
        "PEER_POLL_INTERVAL_MS", "RELEASE_GRACE_MS", "NMOS_ENABLE", "NMOS_REGISTRY_ADDRESS", "NMOS_REGISTRY_PORT",
        "NMOS_QUERY_ADDRESS", "NMOS_QUERY_PORT", "NMOS_POLL_INTERVAL_MS", "NMOS_PORT", "LOCAL_NODE_IDS", "LOCAL_NODE_HOSTNAMES",
        "WEB_PORT", "WEB_ENABLE", "RT_PRIORITY", "CPU_AFFINITY", "LOG_LEVEL", "METRICS_PER_FLOW", "AGENT_CONFIG_FILE"};
}

bool isRuntimeKey(std::string const& key)
{
    static std::set<std::string> const keys = {"PEERS", "MIRROR_MODE", "MIRROR_INCLUDE_DOMAINS", "MIRROR_INCLUDE_FLOWS",
        "MIRROR_EXCLUDE_DOMAINS", "MIRROR_EXCLUDE_FLOWS", "MIRROR_GRACE_S", "TMPFS_RESERVE_MB", "RELEASE_GRACE_MS",
        "PEER_POLL_INTERVAL_MS", "METRICS_PER_FLOW"};
    return keys.count(key) != 0;
}

Config parseConfig(std::map<std::string, std::string> const& values)
{
    Config cfg;
    cfg.host_id = valueOr(values, "HOST_ID", "");
    cfg.mxl_root = valueOr(values, "MXL_ROOT", "/Volumes/mxl");
    cfg.scan_interval_ms = parseInt("SCAN_INTERVAL_MS", valueOr(values, "SCAN_INTERVAL_MS", "2000"), 100, 600000);
    cfg.mirror_mode = valueOr(values, "MIRROR_MODE", "eager");
    if (cfg.mirror_mode != "eager" && cfg.mirror_mode != "on-demand")
    {
        throw ConfigError("MIRROR_MODE must be eager or on-demand");
    }
    cfg.mirror_include_domains = splitComma(valueOr(values, "MIRROR_INCLUDE_DOMAINS", ""));
    cfg.mirror_include_flows = splitComma(valueOr(values, "MIRROR_INCLUDE_FLOWS", ""));
    cfg.mirror_exclude_domains = splitComma(valueOr(values, "MIRROR_EXCLUDE_DOMAINS", ""));
    cfg.mirror_exclude_flows = splitComma(valueOr(values, "MIRROR_EXCLUDE_FLOWS", ""));
    cfg.mirror_grace_s = parseInt("MIRROR_GRACE_S", valueOr(values, "MIRROR_GRACE_S", "10"), 0, 86400);
    cfg.tmpfs_reserve_mb = parseInt("TMPFS_RESERVE_MB", valueOr(values, "TMPFS_RESERVE_MB", "512"), 0, 1024 * 1024);
    cfg.cleanup_mirrors_on_exit = parseBool("CLEANUP_MIRRORS_ON_EXIT", valueOr(values, "CLEANUP_MIRRORS_ON_EXIT", "false"));
    cfg.default_provider = valueOr(values, "DEFAULT_PROVIDER", "verbs");
    if (cfg.default_provider != "verbs" && cfg.default_provider != "tcp")
    {
        throw ConfigError("DEFAULT_PROVIDER must be verbs or tcp");
    }
    cfg.provider_fallback = valueOr(values, "PROVIDER_FALLBACK", "");
    if (!cfg.provider_fallback.empty() && cfg.provider_fallback != "tcp")
    {
        throw ConfigError("PROVIDER_FALLBACK must be empty or tcp");
    }
    cfg.fabric_interface = valueOr(values, "FABRIC_INTERFACE", "");
    cfg.fabric_port_base = parseInt("FABRIC_PORT_BASE", valueOr(values, "FABRIC_PORT_BASE", "23500"), 1, 65535);
    cfg.fabric_port_count = parseInt("FABRIC_PORT_COUNT", valueOr(values, "FABRIC_PORT_COUNT", "100"), 1, 10000);
    if (cfg.fabric_port_base + cfg.fabric_port_count > 65536)
    {
        throw ConfigError("FABRIC_PORT_BASE + FABRIC_PORT_COUNT exceeds 65535");
    }
    cfg.peers = parsePeers(valueOr(values, "PEERS", "[]"));
    cfg.peer_poll_interval_ms = parseInt("PEER_POLL_INTERVAL_MS", valueOr(values, "PEER_POLL_INTERVAL_MS", "2000"), 100, 600000);
    cfg.release_grace_ms = parseInt("RELEASE_GRACE_MS", valueOr(values, "RELEASE_GRACE_MS", "2000"), 0, 600000);
    cfg.nmos_enable = parseBool("NMOS_ENABLE", valueOr(values, "NMOS_ENABLE", "true"));
    cfg.nmos_registry_address = valueOr(values, "NMOS_REGISTRY_ADDRESS", "");
    cfg.nmos_registry_port = parseInt("NMOS_REGISTRY_PORT", valueOr(values, "NMOS_REGISTRY_PORT", "3210"), 1, 65535);
    cfg.nmos_query_address = valueOr(values, "NMOS_QUERY_ADDRESS", "");
    cfg.nmos_query_port = parseInt("NMOS_QUERY_PORT", valueOr(values, "NMOS_QUERY_PORT", "3211"), 1, 65535);
    cfg.nmos_poll_interval_ms = parseInt("NMOS_POLL_INTERVAL_MS", valueOr(values, "NMOS_POLL_INTERVAL_MS", "1000"), 100, 600000);
    cfg.nmos_port = parseInt("NMOS_PORT", valueOr(values, "NMOS_PORT", "3232"), 1, 65534);
    cfg.local_node_ids = splitComma(valueOr(values, "LOCAL_NODE_IDS", ""));
    cfg.local_node_hostnames = splitComma(valueOr(values, "LOCAL_NODE_HOSTNAMES", ""));
    cfg.web_port = parseInt("WEB_PORT", valueOr(values, "WEB_PORT", "8095"), 1, 65535);
    cfg.web_enable = parseBool("WEB_ENABLE", valueOr(values, "WEB_ENABLE", "true"));
    cfg.rt_priority = parseInt("RT_PRIORITY", valueOr(values, "RT_PRIORITY", "0"), 0, 99);
    cfg.cpu_affinity = parseIntList("CPU_AFFINITY", valueOr(values, "CPU_AFFINITY", ""));
    cfg.log_level = valueOr(values, "LOG_LEVEL", "info");
    if (cfg.log_level != "error" && cfg.log_level != "warn" && cfg.log_level != "info" && cfg.log_level != "debug")
    {
        throw ConfigError("LOG_LEVEL must be error, warn, info, or debug");
    }
    cfg.metrics_per_flow = parseBool("METRICS_PER_FLOW", valueOr(values, "METRICS_PER_FLOW", "true"));
    cfg.config_file = valueOr(values, "AGENT_CONFIG_FILE", "");
    if (cfg.web_port == cfg.nmos_port || cfg.web_port == cfg.nmos_port + 1)
    {
        throw ConfigError("WEB_PORT collides with NMOS_PORT");
    }
    if (cfg.host_id.empty())
    {
        if (auto* host = std::getenv("HOSTNAME"))
        {
            cfg.host_id = host;
        }
    }
    if (cfg.host_id.empty())
    {
        throw ConfigError("HOST_ID is empty and hostname is unavailable");
    }
    if (values.count("MIRROR_INCLUDE") || values.count("MIRROR_EXCLUDE"))
    {
        throw ConfigError("MIRROR_INCLUDE and MIRROR_EXCLUDE are not accepted; use MIRROR_INCLUDE_DOMAINS, MIRROR_INCLUDE_FLOWS, MIRROR_EXCLUDE_DOMAINS and MIRROR_EXCLUDE_FLOWS");
    }
    return cfg;
}

Config loadLayered(std::map<std::string, std::string> const& fileValues, std::map<std::string, std::string> const& envValues,
    std::map<std::string, ValueOrigin>* origin)
{
    std::map<std::string, std::string> merged;
    if (origin != nullptr)
    {
        origin->clear();
        for (auto const& key : configKeys())
        {
            (*origin)[key] = ValueOrigin::Default;
        }
    }
    for (auto const& [key, value] : fileValues)
    {
        merged[key] = value;
        if (origin != nullptr)
        {
            (*origin)[key] = ValueOrigin::File;
        }
    }
    for (auto const& [key, value] : envValues)
    {
        merged[key] = value;
        if (origin != nullptr)
        {
            (*origin)[key] = ValueOrigin::Env;
        }
    }
    return parseConfig(merged);
}

std::map<std::string, std::string> configToMap(Config const& cfg)
{
    std::map<std::string, std::string> values;
    values["HOST_ID"] = cfg.host_id;
    values["MXL_ROOT"] = cfg.mxl_root;
    values["SCAN_INTERVAL_MS"] = std::to_string(cfg.scan_interval_ms);
    values["MIRROR_MODE"] = cfg.mirror_mode;
    values["MIRROR_INCLUDE_DOMAINS"] = joinComma(cfg.mirror_include_domains);
    values["MIRROR_INCLUDE_FLOWS"] = joinComma(cfg.mirror_include_flows);
    values["MIRROR_EXCLUDE_DOMAINS"] = joinComma(cfg.mirror_exclude_domains);
    values["MIRROR_EXCLUDE_FLOWS"] = joinComma(cfg.mirror_exclude_flows);
    values["MIRROR_GRACE_S"] = std::to_string(cfg.mirror_grace_s);
    values["TMPFS_RESERVE_MB"] = std::to_string(cfg.tmpfs_reserve_mb);
    values["CLEANUP_MIRRORS_ON_EXIT"] = cfg.cleanup_mirrors_on_exit ? "true" : "false";
    values["DEFAULT_PROVIDER"] = cfg.default_provider;
    values["PROVIDER_FALLBACK"] = cfg.provider_fallback;
    values["FABRIC_INTERFACE"] = cfg.fabric_interface;
    values["FABRIC_PORT_BASE"] = std::to_string(cfg.fabric_port_base);
    values["FABRIC_PORT_COUNT"] = std::to_string(cfg.fabric_port_count);
    values["PEERS"] = peersToJson(cfg.peers);
    values["PEER_POLL_INTERVAL_MS"] = std::to_string(cfg.peer_poll_interval_ms);
    values["RELEASE_GRACE_MS"] = std::to_string(cfg.release_grace_ms);
    values["NMOS_ENABLE"] = cfg.nmos_enable ? "true" : "false";
    values["NMOS_REGISTRY_ADDRESS"] = cfg.nmos_registry_address;
    values["NMOS_REGISTRY_PORT"] = std::to_string(cfg.nmos_registry_port);
    values["NMOS_QUERY_ADDRESS"] = cfg.nmos_query_address;
    values["NMOS_QUERY_PORT"] = std::to_string(cfg.nmos_query_port);
    values["NMOS_POLL_INTERVAL_MS"] = std::to_string(cfg.nmos_poll_interval_ms);
    values["NMOS_PORT"] = std::to_string(cfg.nmos_port);
    values["LOCAL_NODE_IDS"] = joinComma(cfg.local_node_ids);
    values["LOCAL_NODE_HOSTNAMES"] = joinComma(cfg.local_node_hostnames);
    values["WEB_PORT"] = std::to_string(cfg.web_port);
    values["WEB_ENABLE"] = cfg.web_enable ? "true" : "false";
    values["RT_PRIORITY"] = std::to_string(cfg.rt_priority);
    values["CPU_AFFINITY"] = [&] {
        std::string out;
        for (std::size_t i = 0; i < cfg.cpu_affinity.size(); ++i)
        {
            if (i != 0)
            {
                out += ",";
            }
            out += std::to_string(cfg.cpu_affinity[i]);
        }
        return out;
    }();
    values["LOG_LEVEL"] = cfg.log_level;
    values["METRICS_PER_FLOW"] = cfg.metrics_per_flow ? "true" : "false";
    values["AGENT_CONFIG_FILE"] = cfg.config_file;
    return values;
}

std::string configToJson(Config const& cfg)
{
    picojson::object obj;
    for (auto const& [key, value] : configToMap(cfg))
    {
        if (key == "PEERS")
        {
            std::string err;
            auto parsed = json::parse(value, &err);
            obj[key] = err.empty() ? parsed : picojson::value(value);
        }
        else
        {
            obj[key] = picojson::value(value);
        }
    }
    return picojson::value(obj).serialize(true);
}

std::string configToEnv(Config const& cfg)
{
    std::ostringstream out;
    for (auto const& [key, value] : configToMap(cfg))
    {
        out << key << "=" << value << "\n";
    }
    return out.str();
}

std::map<std::string, std::string> environmentValues(char const* const* envp)
{
    auto const keys = configKeys();
    std::set<std::string> known(keys.begin(), keys.end());
    known.insert("MIRROR_INCLUDE");
    known.insert("MIRROR_EXCLUDE");
    std::map<std::string, std::string> out;
    if (envp == nullptr)
    {
        return out;
    }
    for (char const* const* it = envp; *it != nullptr; ++it)
    {
        std::string entry(*it);
        auto const eq = entry.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        auto key = entry.substr(0, eq);
        if (known.count(key) != 0)
        {
            out.emplace(std::move(key), entry.substr(eq + 1));
        }
    }
    return out;
}
} // namespace mfa
