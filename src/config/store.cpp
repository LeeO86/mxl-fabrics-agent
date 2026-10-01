#include "config/store.hpp"

#include "util/jsonutil.hpp"
#include "util/logging.hpp"

#include <fstream>
#include <filesystem>

namespace mfa
{
namespace
{
std::map<std::string, std::string> jsonObjectToMap(std::string const& text)
{
    std::map<std::string, std::string> out;
    if (text.empty())
    {
        return out;
    }
    std::string err;
    auto const root = json::parse(text, &err);
    if (!err.empty() || !root.is<picojson::object>())
    {
        throw ConfigError("config file is not a JSON object");
    }
    for (auto const& [key, value] : root.get<picojson::object>())
    {
        if (value.is<std::string>())
        {
            out[key] = value.get<std::string>();
        }
        else if (value.is<bool>())
        {
            out[key] = value.get<bool>() ? "true" : "false";
        }
        else if (value.is<double>())
        {
            auto const n = value.get<double>();
            if (n == static_cast<double>(static_cast<long long>(n)))
            {
                out[key] = std::to_string(static_cast<long long>(n));
            }
            else
            {
                out[key] = std::to_string(n);
            }
        }
        else
        {
            out[key] = value.serialize();
        }
    }
    return out;
}
} // namespace

ConfigStore::ConfigStore(Config cfg, std::map<std::string, ValueOrigin> origin, std::map<std::string, std::string> fileLayer)
    : cfg_(std::move(cfg))
    , origin_(std::move(origin))
    , file_(std::move(fileLayer))
{
    for (auto const& [key, where] : origin_)
    {
        if (where == ValueOrigin::Env)
        {
            auto const map = configToMap(cfg_);
            auto const it = map.find(key);
            if (it != map.end())
            {
                env_[key] = it->second;
            }
        }
    }
}

Config ConfigStore::get() const
{
    std::lock_guard const lock{mu_};
    return cfg_;
}

std::map<std::string, ValueOrigin> ConfigStore::origins() const
{
    std::lock_guard const lock{mu_};
    return origin_;
}

bool ConfigStore::restartRequired() const
{
    std::lock_guard const lock{mu_};
    return restart_;
}

Config ConfigStore::updateFile(std::map<std::string, std::string> const& patch, bool* restart)
{
    std::lock_guard const lock{mu_};
    for (auto const& [key, value] : patch)
    {
        auto const it = origin_.find(key);
        if (it != origin_.end() && it->second == ValueOrigin::Env)
        {
            throw ConfigError(key + " is set by the environment and is read-only");
        }
        file_[key] = value;
        if (!isRuntimeKey(key))
        {
            restart_ = true;
        }
    }
    cfg_ = loadLayered(file_, env_, &origin_);
    persistUnlocked();
    if (restart != nullptr)
    {
        *restart = restart_;
    }
    return cfg_;
}

void ConfigStore::replaceFile(std::map<std::string, std::string> const& fileLayer)
{
    std::lock_guard const lock{mu_};
    file_ = fileLayer;
    for (auto const& [key, _] : file_)
    {
        if (origin_.count(key) && origin_[key] == ValueOrigin::Env)
        {
            throw ConfigError(key + " is set by the environment and is read-only");
        }
        if (!isRuntimeKey(key))
        {
            restart_ = true;
        }
    }
    cfg_ = loadLayered(file_, env_, &origin_);
    persistUnlocked();
}

std::string ConfigStore::exportJson() const
{
    std::lock_guard const lock{mu_};
    return configToJson(cfg_);
}

std::string ConfigStore::exportEnv() const
{
    std::lock_guard const lock{mu_};
    return configToEnv(cfg_);
}

void ConfigStore::persistUnlocked()
{
    if (cfg_.config_file.empty())
    {
        return;
    }
    picojson::object obj;
    for (auto const& [key, value] : file_)
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
    auto const path = std::filesystem::path(cfg_.config_file);
    auto const tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
        {
            throw ConfigError("cannot write " + tmp);
        }
        out << picojson::value(obj).serialize(true);
    }
    std::filesystem::rename(tmp, path);
    log::info("config_saved", {{"path", path.string()}});
}

std::map<std::string, std::string> loadConfigFile(std::string const& path)
{
    if (path.empty())
    {
        return {};
    }
    std::ifstream in(path);
    if (!in)
    {
        throw ConfigError("cannot read config file " + path);
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return jsonObjectToMap(buffer.str());
}
} // namespace mfa
