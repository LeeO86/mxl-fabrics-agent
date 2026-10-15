#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mfa
{
// Called on the domain thread with a source grain's transfer time (transfer to completion).
using TransferObserver = std::function<void(std::string const& provider, double seconds)>;

// Callbacks from a lane's fabric thread (each may be empty).
struct FabricObservers
{
    TransferObserver transfer;
    // Continuous flows, seconds behind the current TAI sample index: "origin" (the origin's head when the source
    // sends), "sent" (the newest sample of a transfer when the source posts it) and "delivered" (the newest sample
    // of a batch when the destination commits it).
    std::function<void(std::string const& stage, std::string const& flowId, double seconds)> sampleAge;
    // Continuous flows at the destination: the time since the last pass that committed samples, at each pass that
    // commits samples (steady about one transfer period; a long one is a burst).
    std::function<void(std::string const& flowId, double seconds)> deliveryGap;
    // Continuous flows at the source: a transfer from its post to the pass that saw it complete. One transfer
    // is in flight at a time, so a long one (a retransmission timeout, receiver-not-ready retries) holds
    // every later sample back.
    std::function<void(std::string const& provider, std::string const& flowId, double seconds)> sampleTransfer;
    // A pass of the lane's thread came this long after the one before (above the stall threshold), and the
    // slowest call into the lane in between ("pass" when none ran).
    std::function<void(std::string const& flowId, std::string const& task, double seconds)> stall;
};

struct FabricEndpoint
{
    std::string provider = "tcp";
    std::string node;
    std::string service;
    bool allowTcpFallback = false;
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
    double behind_ms = 0;     // destination: the same distance in milliseconds (indexes at the flow's rate)
    std::uint64_t gaps = 0;   // source: origin indexes passed over because the writer never wrote them
    std::string last_error;
    int cq_depth = 0;
    bool fallback = false;
    bool closed = false; // destination: the target was released after a failed read and waits to be set up again
};

struct ProviderProbe
{
    bool ok = false;
    std::vector<std::string> providers;
    std::string error;
};

ProviderProbe probeFabrics(std::string const& scratchDir);

// One lane: the fabric thread, MXL instance and fabrics instance of one flow of one domain (its writer, its
// targets or its initiator). Until 1.7.1 one thread served every flow of a domain, and a slow call for one
// flow (a target or initiator set up again, memory registered, a writer opened) held up the transfers of
// all the others (docs: "Lanes").
class FabricDomain
{
public:
    FabricDomain(std::string path, std::string flowId, int rtPriority, std::vector<int> cpus);
    ~FabricDomain();

    std::string const& path() const { return path_; }
    std::string const& flowId() const { return flowId_; }
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
    // Head index of the flow a source replication reads (also while its transfers are stuck).
    std::optional<std::uint64_t> originHead(std::string const& key);
    // Shutdown without releasing writers or the MXL instance (MXL would delete the flows).
    void keepFlowsOnExit();
    void setObservers(FabricObservers observers);
    // No writer, target or initiator is left: the lane can go.
    bool idle();

    std::vector<FabricRow> rows() const;

private:
    struct Impl;
    std::string path_;
    std::string flowId_;
    bool ok_ = false;
    std::string error_;
    std::unique_ptr<Impl> impl_;
};
} // namespace mfa
