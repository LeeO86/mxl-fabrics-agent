#pragma once

#include "fabric/lanestats.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mfa
{
// The MXL instance of one domain, shared by the lanes of its flows. Each MXL instance has its own inotify
// instance (DomainWatcher), and fs.inotify.max_user_instances (128 by default) counts them per user on the
// whole host, so the agent keeps one per domain as before 1.7.2. MXL 218ddaa locks the instance's reader and
// writer tables and the watcher; a lane only uses its own flow's reader or writer, never a sync group.
struct SharedInstance;
std::shared_ptr<SharedInstance> openSharedInstance(std::string const& path, std::string* error);

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

// AUDIO_PASS_MS (1 to 4, default 2 as 1.7.1; 1 is 1.7.2's timing): the pass interval of a continuous lane, and the
// least time from the pass that saw a sample transfer complete to the next one (at least 2 ms). Set at start.
void setAudioPassMs(int ms);
int audioPassMs();

// One lane: the fabric thread and Fabrics instance of one flow of one domain (its writer, its targets or its
// initiator), on the domain's shared MXL instance. Until 1.7.1 one thread served every flow of a domain, and a
// slow call for one flow (a target or initiator set up again, memory registered, a writer opened) held up the
// transfers of all the others.
class FabricDomain
{
public:
    FabricDomain(std::string path, std::string flowId, std::shared_ptr<SharedInstance> instance, int rtPriority, std::vector<int> cpus);
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
    // No writer, target or initiator is left: the lane can go. `seq` is the call count of this check: when the
    // count is still the same later, no call came in since (callCount).
    bool idle(std::uint64_t* seq);
    std::uint64_t callCount() const;
    // Delivery timing of the lane (a copy, refreshed by its thread about every 100 ms).
    LaneStats stats() const;
    // The counts up to now, read on the lane's thread (for a lane that goes).
    LaneStats finalStats();

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
