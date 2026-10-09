#pragma once

#include "inventory/model.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace mfa
{
Inventory scanOnce(std::string const& root, std::string const& hostId);

class DomainScanner
{
public:
    using Wake = std::function<void()>;

    DomainScanner(std::string root, std::string hostId, int intervalMs, Wake wake);
    ~DomainScanner();

    Inventory snapshot() const;
    bool running() const { return running_.load(); }
    void start();
    void stop();

private:
    void loop();

    std::string root_;
    std::string host_;
    int intervalMs_;
    Wake wake_;
    mutable std::mutex mu_;
    Inventory inventory_;
    std::string lastCanonical_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
} // namespace mfa
