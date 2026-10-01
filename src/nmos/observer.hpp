#pragma once

#include "config/config.hpp"
#include "demand/derive.hpp"
#include "peer/manager.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mfa
{
struct NmosSnapshot
{
    bool registry_up = false;
    bool registered = false;
    std::string registry = "disabled";
    std::string error;
    std::vector<ReceiverView> receivers;
    std::vector<PeerSnapshot> agents;
    std::uint64_t poll_errors = 0;
};

class NmosObserver
{
public:
    using Wake = std::function<void()>;

    NmosObserver(Config cfg, LocalIdentity self, Wake wake);
    ~NmosObserver();
    void updateConfig(Config const& cfg);
    void setRegistered(bool ready);
    NmosSnapshot snapshot() const;
    void start();
    void stop();

private:
    void loop();
    bool discover(std::string& host, int& port);
    void poll(std::string const& base);

    Config cfg_;
    LocalIdentity self_;
    Wake wake_;
    mutable std::mutex mu_;
    NmosSnapshot snap_;
    bool stop_ = false;
    std::thread thread_;
};
} // namespace mfa
