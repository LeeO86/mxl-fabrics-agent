#pragma once

#include "config/config.hpp"
#include "inventory/model.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mfa
{
struct PeerSnapshot
{
    std::string host_id;
    std::string control_url;
    std::string provider;
    std::string local_addr;
    std::string remote_addr;
    bool up = false;
    std::string boot_id;
    std::uint64_t revision = 0;
    Inventory inventory;
    std::string error;
};

Inventory parseInventoryJson(std::string const& body);

class PeerManager
{
public:
    using Wake = std::function<void()>;

    PeerManager(Config cfg, Wake wake);
    ~PeerManager();
    void updateConfig(Config const& cfg);
    void setDiscovered(std::vector<PeerSnapshot> const& discovered);
    std::vector<PeerSnapshot> snapshot() const;
    void start();
    void stop();

private:
    void loop();
    std::vector<PeerSnapshot> mergeUnlocked() const;

    Config cfg_;
    Wake wake_;
    mutable std::mutex mu_;
    std::vector<PeerSnapshot> discovered_;
    std::vector<PeerSnapshot> live_;
    bool stop_ = false;
    std::thread thread_;
};
} // namespace mfa
