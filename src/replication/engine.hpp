#pragma once

#include "config/config.hpp"
#include "fabric/domain.hpp"
#include "replication/handshake.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mfa
{
struct PullRequest
{
    std::string domain_id;
    std::string flow_id;
    std::string flow_def_json;
    std::string mirror_path;
    std::string peer;
    std::string control_url;
    std::string local_addr;
    std::string provider;
    bool allow_tcp_fallback = false;
    std::string peer_boot;
    std::uint64_t peer_revision = 0;
    bool source_active = false; // a writer holds the origin flow (peer inventory): grains are expected
};

struct ReplicaView
{
    std::string replication_id;
    std::string role;
    std::string domain_id;
    std::string flow_id;
    std::string peer;
    std::string provider;
    std::string state;
    std::uint64_t grains = 0;
    std::uint64_t bytes = 0;
    std::uint64_t errors = 0;
    std::uint64_t restarts = 0;
    std::uint64_t head = 0;
    std::int64_t lag = 0;
    double lag_ms = 0; // destination: the lag in milliseconds at the flow's rate
    std::uint64_t gaps = 0; // source: origin indexes never written, passed over
    std::string last_error;
    bool fallback = false;
    int cq_depth = 0;
};

struct LaneStatsSnapshot
{
    std::vector<LaneStats> live;
    std::vector<LaneStats> retired;
};

class ReplicationEngine
{
public:
    explicit ReplicationEngine(Config cfg);
    ~ReplicationEngine();

    void updateConfig(Config const& cfg);
    // Peers whose local fabric link is down, with the reason: their replications show link_down, and
    // a destination drops its targets for them until the link is back.
    void setLinksDown(std::map<std::string, std::string> down);
    void setPulls(std::vector<PullRequest> const& pulls);
    PostResult post(PostRequest const& request, std::string const& domainPath);
    bool eraseTarget(std::string const& replicationId, std::string const& destHostId);
    std::vector<ReplicaView> status() const;
    // The lane of one flow in a domain (opened when it does not exist yet). Each flow has its own fabric thread.
    std::shared_ptr<FabricDomain> domain(std::string const& path, std::string const& flowId);
    // Every lane of a domain.
    void releaseDomain(std::string const& path);
    // Drops the lanes with nothing left in them (a safety net for the drops after each release; once per tick).
    std::size_t sweepIdleLanes();
    std::size_t laneCount() const;
    // The stats of the lanes there are, and the final stats of those that went since the last call.
    LaneStatsSnapshot takeLaneStats();
    void releaseAll(bool keepFlows = false);
    bool ensureWriter(std::string const& path, std::string const& flowDef, std::string* error);
    void releaseWriter(std::string const& path, std::string const& flowId);
    // Before a mirror flow is rebuilt: drops its destination targets (and the sources' connections to them),
    // so the next setPulls sets up new ones on the new writer, then releases the agent's writer.
    void resetMirrorFlow(std::string const& path, std::string const& domainId, std::string const& flowId);

private:
    struct DestState
    {
        std::string replication_id;
        std::string target_info;
        std::string peer_boot;
        std::string mirror_path;
        std::string control_url;
        std::uint64_t peer_revision = 0;
        std::string state = "pending";
        std::chrono::steady_clock::time_point next_attempt{};
        std::chrono::milliseconds backoff{250};
        std::uint64_t restarts = 0;
        std::string last_error;
        std::uint64_t seen_grains = 0;                         // destination row's grains at the last check
        std::chrono::steady_clock::time_point progress_at{};   // when they last changed (or the target was set up)
        std::uint64_t stalls = 0;                              // rebuilds in a row without a new grain
        bool source_active = false;                            // from the last pull request
        std::optional<std::uint64_t> origin_head;              // from the source's handshake answer (1.1.0 sources)
        std::chrono::steady_clock::time_point origin_moved_at{}; // when that head last changed
        bool source_error = false;                             // the source's last answer was "error"
        bool link_down = false;                                // torn down for a dead local fabric link
        bool fallback = false;
        std::string provider;
    };

    std::string endpointFor(std::string const& peer, std::string* provider, bool* fallback) const;
    FabricEndpoint endpoint(std::string const& peer) const;
    // The lane if it exists (nothing is opened).
    std::shared_ptr<FabricDomain> findLane(std::string const& path, std::string const& flowId) const;
    // Drops the lane when nothing is left in it (no writer, target or initiator) and nobody else holds it.
    bool dropIfIdle(std::string const& path, std::string const& flowId);
    std::shared_ptr<SharedInstance> sharedInstance(std::string const& path, std::string* error);
    std::vector<std::shared_ptr<FabricDomain>> lanes() const;
    // Final stats of lanes that left the map (into retired_), then the lanes go.
    void retire(std::vector<std::shared_ptr<FabricDomain>> lanes);

    mutable std::mutex mu_;
    Config cfg_;
    HandshakeTable handshake_;
    std::map<std::string, std::shared_ptr<FabricDomain>> domains_; // by laneKey(path, flow id)
    std::map<std::string, DestState> dest_;
    std::map<std::string, std::string> linksDown_;
    std::map<std::string, std::uint64_t> sourceRestarts_;
    std::map<std::string, std::string> laneErrors_; // path → last logged lane error (under mu_)
    std::vector<std::shared_ptr<FabricDomain>> retiring_; // out of the map, final stats not taken yet (under mu_)
    std::vector<LaneStats> retired_;                      // final stats for takeLaneStats (under mu_)
    std::mutex instancesMu_;
    std::map<std::string, std::weak_ptr<SharedInstance>> instances_; // one MXL instance per domain path
    int nextPort_;
};
} // namespace mfa
