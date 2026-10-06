#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mfa
{
// Called on the domain thread with a source grain's transfer time (first transfer to completion).
using TransferObserver = std::function<void(std::string const& provider, double seconds)>;

struct FabricEndpoint
{
    std::string provider = "tcp";
    std::string node;
    std::string service;
    bool allowTcpFallback = false;
    int pacingBatches = 0;     // TRANSFER_PACING=frame: slice batches per grain; 0 = whole grains
    double pacingSpread = 0.5; // share of the grain duration the batches start within
};

struct TargetSetup
{
    bool ok = false;
    std::string target_info;
    std::string provider_used;
    bool fallback = false;
    std::string error;
};

struct FabricRow
{
    std::string key;
    std::string role;
    std::string flow_id;
    std::string peer;
    std::string provider;
    std::string state;
    std::uint64_t grains = 0;
    std::uint64_t bytes = 0;
    std::uint64_t errors = 0;
    std::uint64_t head = 0;
    std::uint64_t behind = 0; // destination: grains from head to the flow's current TAI index (0 before the first grain)
    std::string last_error;
    int cq_depth = 0;
    bool fallback = false;
};

struct ProviderProbe
{
    bool ok = false;
    std::vector<std::string> providers;
    std::string error;
};

ProviderProbe probeFabrics(std::string const& scratchDir);

class FabricDomain
{
public:
    FabricDomain(std::string path, int rtPriority, std::vector<int> cpus);
    ~FabricDomain();

    std::string const& path() const { return path_; }
    bool ok() const { return ok_; }
    std::string error() const { return error_; }

    bool ensureWriter(std::string const& flowDefJson, std::string* error);
    void releaseWriter(std::string const& flowId);
    bool commitPattern(std::string const& flowId, std::uint64_t* index, std::string* error);
    std::uint64_t headIndex(std::string const& flowId);

    TargetSetup setupTarget(std::string const& key, std::string const& flowId, FabricEndpoint const& endpoint, int cqDepth);
    void destroyTarget(std::string const& key);

    bool ensureInitiator(std::string const& key, std::string const& flowId, FabricEndpoint const& endpoint, std::string* error);
    bool addInitiatorTarget(std::string const& key, std::string const& destHost, std::string const& targetInfo, std::string* error);
    void removeInitiatorTarget(std::string const& key, std::string const& destHost);
    void destroyInitiator(std::string const& key);
    // Shutdown without releasing writers or the MXL instance (MXL would delete the flows).
    void keepFlowsOnExit();
    void setTransferObserver(TransferObserver observer);

    std::vector<FabricRow> rows() const;

private:
    struct Impl;
    std::string path_;
    bool ok_ = false;
    std::string error_;
    std::unique_ptr<Impl> impl_;
};
} // namespace mfa
