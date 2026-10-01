#pragma once

#include "config/config.hpp"

#include <mutex>
#include <string>

namespace mfa
{
class ConfigStore
{
public:
    ConfigStore(Config cfg, std::map<std::string, ValueOrigin> origin, std::map<std::string, std::string> fileLayer);

    Config get() const;
    std::map<std::string, ValueOrigin> origins() const;
    bool restartRequired() const;

    // Apply a partial file-layer update. Env-owned keys are rejected.
    // Returns the merged config. Runtime keys take effect immediately.
    Config updateFile(std::map<std::string, std::string> const& patch, bool* restart = nullptr);
    void replaceFile(std::map<std::string, std::string> const& fileLayer);
    std::string exportJson() const;
    std::string exportEnv() const;

private:
    void persistUnlocked();

    mutable std::mutex mu_;
    Config cfg_;
    std::map<std::string, ValueOrigin> origin_;
    std::map<std::string, std::string> file_;
    std::map<std::string, std::string> env_;
    bool restart_ = false;
};
} // namespace mfa
