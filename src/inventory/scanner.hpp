#pragma once

#include "inventory/model.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace mfa
{
// The header of a flow's data file: ring depth, head index (runtime.headIndex, MXL_UNDEFINED_INDEX or 0
// before the first grain) and the file's inode (0 when it does not exist).
struct FlowHeader
{
    std::uint32_t grainCount = 0;
    std::uint64_t headIndex = 0;
    std::uint64_t inode = 0;
};

FlowHeader readFlowHeader(std::string const& dataFile);

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
