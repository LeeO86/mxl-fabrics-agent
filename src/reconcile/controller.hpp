#pragma once

#include "config/store.hpp"
#include "demand/derive.hpp"
#include "inventory/scanner.hpp"
#include "mirror/health.hpp"
#include "mirror/lifecycle.hpp"
#include "mirror/readers.hpp"
#include "nmos/node.hpp"
#include "nmos/observer.hpp"
#include "ops/httpserver.hpp"
#include "ops/metrics.hpp"
#include "peer/manager.hpp"
#include "replication/engine.hpp"
#include "util/net.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace mfa
{
class Controller
{
public:
    explicit Controller(std::shared_ptr<ConfigStore> store);
    ~Controller();

    void start();
    void stop();
    bool ready() const { return ready_.load(); }
    int fatal() const { return fatal_.load(); }
    HttpResponse handle(HttpRequest const& request);
    void stream(SseEmit const& emit);

private:
    void loop();
    void tick();
    void publish(std::string const& event, std::string const& data);
    void renderMetrics();
    HttpResponse testPeer(std::string const& hostId);

    // One destination mirror domain, for the cleanup and GET /api/v1/mirrors.
    struct MirrorDomainView
    {
        std::string domain_id;
        std::string path;
        std::string source_host_id;
        bool on_disk = false;
        bool source_up = false;
        bool origin_on_peer = false;
        bool origin_local = false;
        bool demand = false;
        std::string state;
        std::int64_t created_unix = 0;    // mtime of its domain_def.json, 0 = unknown
        std::int64_t idle_since_unix = 0; // since when it has had no demand, 0 = it has
        std::vector<std::pair<FlowRecord, std::string>> flows; // with the per-flow mirror state
    };
    std::vector<MirrorDomainView> cleanupMirrors(Config const& cfg, Inventory const& inventory, std::vector<PeerSnapshot> const& peerSnap,
        bool truthOk, std::set<std::string> const& localIds, std::set<std::string> const& peerDomains,
        std::set<std::string> const& wanted, std::vector<DomainRecord> const& flowViews);
    // Closes the agent's writers of a mirror domain and removes it when no other process maps it.
    // Returns "" when removed, else why not (with the readers).
    std::string removeMirrorDomain(std::string const& domainId, std::string const& path, std::string const& reason,
        std::vector<ReaderProcess>* readers);
    // Mirror flows whose origin flow is gone, in mirror domains that stay (other flows still in use): removed
    // after MIRROR_IDLE_GRACE_S when no other process maps one of their files.
    void cleanupMirrorFlows(Config const& cfg, Inventory const& inventory, std::vector<PeerSnapshot> const& peerSnap,
        bool truthOk, std::set<std::string> const& seen);
    // Tears one mirror flow down for a rebuild (under tickMu_): its targets and the sources' connections, the
    // agent's writer and its files. The next file of the flow is a rebuild for `reason`. Readers that still
    // hold the old file get MXL_ERR_FLOW_INVALID once no new grain comes, and open the new one.
    void teardownMirrorFlow(std::string const& path, std::string const& domainId, std::string const& flowId, std::string const& reason);
    // scanReaders on /proc for every other process, timed (reconcile_work_seconds{work="reader_scan"}).
    ReaderScan scanProcReaders(std::map<FileKey, std::string> const& files);
    // Duration of one piece of periodic work: histogram, debug line, an info line when it takes over 0.5 s.
    void noteWork(char const* work, std::chrono::steady_clock::duration took);
    void noteWork(char const* work, double seconds);
    HttpResponse rebuildMirror(MirrorRebuildRequest const& request);

    std::shared_ptr<ConfigStore> store_;
    std::string bootId_;
    ReplicationEngine engine_;
    DomainScanner scanner_;
    PeerManager peers_;
    NmosObserver observer_;
    std::unique_ptr<NmosNode> node_;
    Metrics metrics_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> metricsPerFlow_{true}; // METRICS_PER_FLOW, for the lanes' sample age observations
    std::atomic<bool> nmosReady_{false};
    std::atomic<bool> nmosRegistered_{false};
    std::atomic<int> fatal_{0};
    std::string nmosError_;
    DemandSnapshot demand_;
    std::vector<DomainRecord> mirrors_;
    std::map<std::string, std::chrono::steady_clock::time_point> originLost_;
    std::map<std::string, std::chrono::steady_clock::time_point> demandLost_;
    std::set<std::string> lastDemand_;
    std::mutex tickMu_; // one tick or one DELETE /api/v1/mirrors at a time
    std::chrono::steady_clock::time_point started_{};
    std::chrono::steady_clock::time_point nextReaderScan_{};
    std::map<std::string, std::chrono::steady_clock::time_point> mirrorIdleSince_; // per mirror domain without demand
    std::map<std::string, std::string> mirrorLogged_; // per mirror domain, the last logged cleanup decision
    std::vector<MirrorDomainView> mirrorDomains_;     // from the last tick (mu_)
    std::map<std::string, MissingClock> flowMissingSince_; // per "domain/flow" without its origin
    // Which peers' inventories may decide about flows and domains they do not list (1.7.1), and the up peers
    // that list each domain, both from the current pass.
    InventorySettleTracker settle_;
    std::map<std::string, std::set<std::string>> domainHolders_;
    // The source peer of a mirror domain counts as up for the clean-up and health decisions.
    bool sourceCounts(std::string const& sourceHost) const;
    std::map<std::string, std::string> flowLogged_; // per "domain/flow", the last logged flow cleanup decision
    // Per "domain/flow", when the mirror was last rebuilt for a changed origin definition (at most one per 5 s;
    // entries older than that are dropped every pass).
    std::map<std::string, std::chrono::steady_clock::time_point> configRebuiltAt_;
    bool adminRebuild_ = false; // MIRROR_ADMIN_REBUILD at start
    std::chrono::steady_clock::time_point nextFlowReaderScan_{};
    std::mutex workLogMu_;
    std::chrono::steady_clock::time_point slowWorkLogged_{}; // under workLogMu_
    std::map<std::string, FabricLink> links_;  // per peer, from the last tick (mu_)
    std::map<std::string, std::string> linkErrors_; // per peer, the last logged link error ("" = up)
    struct OriginConflict
    {
        std::string hosts;  // the peers that hold the flow
        std::string chosen; // the one replicated from
    };
    std::map<std::string, OriginConflict> originConflicts_; // per "domain/flow" held by more than one peer
    // Stale and rebuilt destination mirror flows. The tracker is used under tickMu_, its copy for the API
    // and the metrics under mu_.
    void observeMirrors(Config const& cfg, Inventory const& inventory, std::vector<PeerSnapshot> const& peerSnap,
        std::vector<MirrorObservation> observations, std::set<std::string> const& seen);
    void reportMirrorEvents(std::vector<MirrorEvent> const& events, int staleMs);
    MirrorHealthTracker mirrorHealth_;
    std::map<std::string, MirrorHealth> mirrorHealthView_;
    struct Event
    {
        std::string name;
        std::string data;
    };
    std::mutex eventMu_;
    std::condition_variable eventCv_;
    std::vector<Event> events_;
    bool eventsStop_ = false;
    int tai_ = 0;
    bool rootTmpfs_ = false;
};
} // namespace mfa
