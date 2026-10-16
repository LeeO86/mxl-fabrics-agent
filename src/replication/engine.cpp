#include "replication/engine.hpp"

#include "replication/policy.hpp"
#include "util/httpclient.hpp"
#include "util/jsonutil.hpp"
#include "util/logging.hpp"

#include <algorithm>
#include <filesystem>
#include <utility>

namespace mfa
{
namespace
{
std::string pullKey(PullRequest const& pull)
{
    return pull.peer + "/" + pull.domain_id + "/" + pull.flow_id;
}

// The flow id of a destination key (peer/domain/flow).
std::string flowOfKey(std::string const& key)
{
    auto const slash = key.rfind('/');
    return slash == std::string::npos ? key : key.substr(slash + 1);
}

std::string laneKey(std::string const& path, std::string const& flowId)
{
    return path + '\n' + flowId;
}

std::string flowIdOfDef(std::string const& flowDef)
{
    return json::asString(json::objectOrEmpty(json::parse(flowDef)), "id").value_or("");
}
} // namespace

ReplicationEngine::ReplicationEngine(Config cfg)
    : cfg_(std::move(cfg))
    , nextPort_(cfg_.fabric_port_base)
{}

ReplicationEngine::~ReplicationEngine()
{
    domains_.clear();
}

void ReplicationEngine::updateConfig(Config const& cfg)
{
    std::lock_guard const lock{mu_};
    cfg_ = cfg;
}

void ReplicationEngine::setLinksDown(std::map<std::string, std::string> down)
{
    std::lock_guard const lock{mu_};
    linksDown_ = std::move(down);
}

FabricEndpoint ReplicationEngine::endpoint(std::string const& peer) const
{
    FabricEndpoint ep;
    std::lock_guard const lock{mu_};
    ep.allowTcpFallback = cfg_.provider_fallback == "tcp";
    ep.provider = cfg_.default_provider;
    ep.node = cfg_.fabric_interface;
    for (auto const& item : cfg_.peers)
    {
        if (item.host_id == peer)
        {
            if (!item.provider.empty())
            {
                ep.provider = item.provider;
            }
            if (!item.local_fabric_addr.empty())
            {
                ep.node = item.local_fabric_addr;
            }
            break;
        }
    }
    return ep;
}

std::shared_ptr<SharedInstance> ReplicationEngine::sharedInstance(std::string const& path, std::string* error)
{
    std::lock_guard const lock{instancesMu_};
    if (auto existing = instances_[path].lock())
    {
        return existing;
    }
    auto created = openSharedInstance(path, error);
    instances_[path] = created;
    // Forget the paths whose instance has gone.
    for (auto it = instances_.begin(); it != instances_.end();)
    {
        it = it->second.expired() && it->first != path ? instances_.erase(it) : std::next(it);
    }
    return created;
}

std::shared_ptr<FabricDomain> ReplicationEngine::domain(std::string const& path, std::string const& flowId)
{
    auto const laneId = laneKey(path, flowId);
    {
        std::lock_guard const lock{mu_};
        auto const it = domains_.find(laneId);
        if (it != domains_.end())
        {
            return it->second;
        }
    }
    int rt = 0;
    std::vector<int> cpus;
    {
        std::lock_guard const lock{mu_};
        rt = cfg_.rt_priority;
        cpus = cfg_.cpu_affinity;
    }
    std::string instanceError;
    auto instance = sharedInstance(path, &instanceError);
    auto created = std::make_shared<FabricDomain>(path, flowId, instance, rt, cpus);
    if (!created->ok())
    {
        auto const error = instance ? created->error() : instanceError;
        bool logIt = false;
        {
            std::lock_guard const lock{mu_};
            logIt = std::exchange(laneErrors_[path], error) != error;
        }
        // One line per domain while the error stays the same (the lane is set up again on each pass).
        if (logIt)
        {
            log::error("domain_fabric_failed", {{"path", path}, {"flow_id", flowId}, {"error", error}});
        }
        return created; // not kept: the callers fail and the next call tries again
    }
    std::lock_guard const lock{mu_};
    laneErrors_.erase(path);
    auto const it = domains_.find(laneId);
    if (it != domains_.end())
    {
        return it->second;
    }
    domains_[laneId] = created;
    return created;
}

std::shared_ptr<FabricDomain> ReplicationEngine::findLane(std::string const& path, std::string const& flowId) const
{
    std::lock_guard const lock{mu_};
    auto const it = domains_.find(laneKey(path, flowId));
    return it == domains_.end() ? nullptr : it->second;
}

bool ReplicationEngine::dropIfIdle(std::string const& path, std::string const& flowId)
{
    auto lane = findLane(path, flowId);
    std::uint64_t seq = 0;
    if (!lane || !lane->idle(&seq))
    {
        return false;
    }
    std::shared_ptr<FabricDomain> dropped;
    {
        std::lock_guard const lock{mu_};
        auto const it = domains_.find(laneKey(path, flowId));
        // Atomic against a caller that took the lane meanwhile: only the map and this function hold it (lanes are
        // only handed out under mu_), and no call came in since the idle check. Otherwise the next check drops it.
        if (it != domains_.end() && it->second == lane && lane.use_count() == 2 && lane->callCount() == seq)
        {
            dropped = std::move(it->second);
            domains_.erase(it);
        }
    }
    lane.reset();
    // The lane's thread stops here, outside the lock.
    return dropped != nullptr;
}

std::size_t ReplicationEngine::sweepIdleLanes()
{
    std::vector<std::pair<std::string, std::string>> keys;
    {
        std::lock_guard const lock{mu_};
        for (auto const& [_, lane] : domains_)
        {
            keys.emplace_back(lane->path(), lane->flowId());
        }
    }
    std::size_t dropped = 0;
    for (auto const& [path, flowId] : keys)
    {
        dropped += dropIfIdle(path, flowId) ? 1 : 0;
    }
    return dropped;
}

std::size_t ReplicationEngine::laneCount() const
{
    std::lock_guard const lock{mu_};
    return domains_.size();
}

std::vector<LaneStats> ReplicationEngine::laneStats() const
{
    std::vector<LaneStats> out;
    for (auto const& lane : lanes())
    {
        out.push_back(lane->stats());
    }
    return out;
}

std::vector<std::shared_ptr<FabricDomain>> ReplicationEngine::lanes() const
{
    std::vector<std::shared_ptr<FabricDomain>> out;
    std::lock_guard const lock{mu_};
    out.reserve(domains_.size());
    for (auto const& [_, lane] : domains_)
    {
        out.push_back(lane);
    }
    return out;
}

void ReplicationEngine::releaseDomain(std::string const& path)
{
    std::vector<std::shared_ptr<FabricDomain>> dropped;
    {
        std::lock_guard const lock{mu_};
        auto const prefix = path + '\n';
        for (auto it = domains_.begin(); it != domains_.end();)
        {
            if (it->first.compare(0, prefix.size(), prefix) == 0)
            {
                dropped.push_back(std::move(it->second));
                it = domains_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

void ReplicationEngine::releaseAll(bool keepFlows)
{
    std::map<std::string, std::shared_ptr<FabricDomain>> dropped;
    {
        std::lock_guard const lock{mu_};
        dropped.swap(domains_);
    }
    if (keepFlows)
    {
        for (auto const& [_, dom] : dropped)
        {
            dom->keepFlowsOnExit();
        }
    }
    dropped.clear();
}

bool ReplicationEngine::ensureWriter(std::string const& path, std::string const& flowDef, std::string* error)
{
    auto const flowId = flowIdOfDef(flowDef);
    if (flowId.empty())
    {
        if (error != nullptr)
        {
            *error = "flow definition has no id";
        }
        return false;
    }
    auto dom = domain(path, flowId);
    if (!dom || !dom->ok())
    {
        if (error != nullptr)
        {
            *error = dom ? dom->error() : "no domain";
        }
        return false;
    }
    return dom->ensureWriter(flowDef, error);
}

void ReplicationEngine::releaseWriter(std::string const& path, std::string const& flowId)
{
    auto dom = findLane(path, flowId);
    if (!dom)
    {
        return;
    }
    dom->releaseWriter(flowId);
    dom.reset();
    dropIfIdle(path, flowId);
}

void ReplicationEngine::resetMirrorFlow(std::string const& path, std::string const& domainId, std::string const& flowId)
{
    auto const suffix = "/" + domainId + "/" + flowId;
    std::vector<std::pair<std::string, DestState>> reset;
    {
        std::lock_guard const lock{mu_};
        for (auto& [key, state] : dest_)
        {
            if (key.size() <= suffix.size() || key.compare(key.size() - suffix.size(), suffix.size(), suffix) != 0)
            {
                continue;
            }
            reset.emplace_back(key, state);
            auto const restarts = state.restarts + 1;
            state = DestState{};
            state.restarts = restarts;
        }
    }
    for (auto const& [key, state] : reset)
    {
        if (!state.control_url.empty() && !state.replication_id.empty())
        {
            httpRequest("DELETE", state.control_url + "/replications/" + state.replication_id + "/targets/" + cfg_.host_id);
        }
        if (auto dom = findLane(state.mirror_path.empty() ? path : state.mirror_path, flowId))
        {
            dom->destroyTarget(key);
        }
    }
    releaseWriter(path, flowId);
}

void ReplicationEngine::setPulls(std::vector<PullRequest> const& pulls)
{
    std::map<std::string, PullRequest> wanted;
    for (auto const& pull : pulls)
    {
        wanted[pullKey(pull)] = pull;
    }
    std::vector<std::pair<std::string, DestState>> drop;
    std::map<std::string, std::string> linksDown;
    {
        std::lock_guard const lock{mu_};
        linksDown = linksDown_;
        for (auto const& [key, state] : dest_)
        {
            if (wanted.count(key) == 0)
            {
                drop.emplace_back(key, state);
            }
        }
        for (auto const& [key, _] : drop)
        {
            dest_.erase(key);
        }
    }
    for (auto const& [key, state] : drop)
    {
        if (!state.control_url.empty() && !state.replication_id.empty())
        {
            httpRequest("DELETE", state.control_url + "/replications/" + state.replication_id + "/targets/" + cfg_.host_id);
        }
        if (!state.mirror_path.empty())
        {
            if (auto dom = findLane(state.mirror_path, flowOfKey(key)))
            {
                dom->destroyTarget(key);
            }
            dropIfIdle(state.mirror_path, flowOfKey(key)); // the writer may be gone already (plan.remove)
        }
        log::info("replication_released", {{"peer", key.substr(0, key.find('/'))}, {"replication_id", state.replication_id}});
    }

    for (auto const& [key, pull] : wanted)
    {
        auto& state = dest_[key];
        state.mirror_path = pull.mirror_path;
        state.control_url = pull.control_url;
        bool const peerRestarted = !state.peer_boot.empty() && state.peer_boot != pull.peer_boot;
        bool const revisionReset = state.peer_revision != 0 && pull.peer_revision < state.peer_revision;
        if (peerRestarted || revisionReset)
        {
            log::info("peer_restarted", {{"peer", pull.peer}, {"flow_id", pull.flow_id}});
            auto dom = findLane(pull.mirror_path, pull.flow_id);
            if (dom)
            {
                dom->destroyTarget(key);
            }
            if (!pull.control_url.empty() && !state.replication_id.empty())
            {
                httpRequest("DELETE", pull.control_url + "/replications/" + state.replication_id + "/targets/" + cfg_.host_id);
            }
            state = DestState{};
            state.restarts += 1;
        }
        state.peer_boot = pull.peer_boot;
        state.peer_revision = pull.peer_revision;
        state.source_active = pull.source_active;
        // A dead local fabric link (no carrier): drop the target and the source's connection now, and
        // set up fresh ones when the link is back. No retries meanwhile; the peer's link_down is logged once.
        if (auto const down = linksDown.find(pull.peer); down != linksDown.end())
        {
            if (!state.link_down)
            {
                if (auto dom = findLane(pull.mirror_path, pull.flow_id))
                {
                    dom->destroyTarget(key);
                }
                if (!pull.control_url.empty() && !state.replication_id.empty())
                {
                    httpRequest("DELETE", pull.control_url + "/replications/" + state.replication_id + "/targets/" + cfg_.host_id);
                }
                state.target_info.clear();
                state.stalls = 0;
                state.backoff = std::chrono::milliseconds(250);
                state.link_down = true;
            }
            state.state = "link_down";
            state.last_error = down->second;
            continue;
        }
        if (std::exchange(state.link_down, false))
        {
            state.state = "pending";
            state.last_error.clear();
            state.next_attempt = {};
        }
        if (std::chrono::steady_clock::now() < state.next_attempt && state.state == "error")
        {
            continue;
        }
        auto dom = domain(pull.mirror_path, pull.flow_id);
        if (!dom || !dom->ok())
        {
            state.state = "error";
            state.last_error = dom ? dom->error() : "domain";
            continue;
        }
        // A destination without a new grain while it is in error, the source reports an error (a transfer
        // that does not complete) or the origin moved on (a peer that hung, a queue pair out of retries, a
        // closed connection that reports nothing) is set up again: the source gets new target info and
        // replaces the dead connection. See shouldRebuild for the waits. "Moved on" is the origin head the
        // source reports in the handshake; a holder that writes nothing (decklink without a signal) is
        // not a dead link. Sources before 1.1.0 report no head: a writer counts. A source whose own link
        // to this host is down (link_down) waits for the link.
        if (!state.target_info.empty() && state.state != "link_down")
        {
            auto const now = std::chrono::steady_clock::now();
            for (auto const& row : dom->rows())
            {
                if (row.key != key || row.role != "destination")
                {
                    continue;
                }
                StallInput stall;
                stall.sinceGrain = now - state.progress_at;
                stall.stalls = state.stalls;
                stall.destError = row.state == "error";
                stall.sourceError = state.source_error;
                stall.destClosed = row.closed;
                // "Moved on": more than 1 s after the last grain, so the head read in the same pass as
                // that grain does not count.
                stall.originMoved = state.origin_head ? state.origin_moved_at > state.progress_at + std::chrono::seconds(1) : pull.source_active;
                if (row.grains != state.seen_grains)
                {
                    state.seen_grains = row.grains;
                    state.progress_at = now;
                    state.stalls = 0;
                }
                else if (shouldRebuild(stall))
                {
                    log::warn("replication_restarted", {{"peer", pull.peer}, {"flow_id", pull.flow_id}, {"state", row.state},
                                                           {"error", state.source_error ? state.last_error : row.last_error}});
                    dom->destroyTarget(key);
                    state.target_info.clear();
                    state.source_error = false;
                    state.restarts += 1;
                    state.stalls += 1;
                }
            }
        }
        if (state.target_info.empty())
        {
            FabricEndpoint ep = endpoint(pull.peer);
            if (!pull.local_addr.empty())
            {
                ep.node = pull.local_addr;
            }
            if (!pull.provider.empty())
            {
                ep.provider = pull.provider;
            }
            ep.allowTcpFallback = pull.allow_tcp_fallback;
            int port = 0;
            {
                std::lock_guard const lock{mu_};
                port = nextPort_++;
                if (nextPort_ >= cfg_.fabric_port_base + cfg_.fabric_port_count)
                {
                    nextPort_ = cfg_.fabric_port_base;
                }
            }
            ep.service = std::to_string(port);
            auto const setup = dom->setupTarget(key, pull.flow_id, ep, 256);
            if (!setup.ok)
            {
                // One line when the error starts or changes, not one per retry.
                if (state.state != "error" || state.last_error != setup.error)
                {
                    log::warn("target_setup_failed", {{"flow_id", pull.flow_id}, {"peer", pull.peer}, {"error", setup.error}});
                }
                state.state = "error";
                state.last_error = setup.error;
                state.backoff = nextBackoff(state.backoff);
                state.next_attempt = std::chrono::steady_clock::now() + state.backoff;
                state.restarts += 1;
                continue;
            }
            state.target_info = setup.target_info;
            state.seen_grains = 0;
            state.progress_at = std::chrono::steady_clock::now();
            state.provider = setup.provider_used;
            state.fallback = setup.fallback;
            state.state = "pending";
        }
        if (pull.control_url.empty())
        {
            state.state = "error";
            state.last_error = "peer has no control url";
            continue;
        }
        picojson::object body;
        body["domain_id"] = picojson::value(pull.domain_id);
        body["flow_id"] = picojson::value(pull.flow_id);
        body["dest_host_id"] = picojson::value(cfg_.host_id);
        body["target_info"] = picojson::value(state.target_info);
        auto const response = httpRequest("POST", pull.control_url + "/replications", picojson::value(body).serialize());
        if (response.status != 200 && response.status != 201)
        {
            state.state = "error";
            state.source_error = false;
            auto const reason = json::asString(json::objectOrEmpty(json::parse(response.body)), "error").value_or("");
            state.last_error = !response.error.empty() ? response.error
                                                       : "http " + std::to_string(response.status) + (reason.empty() ? "" : ": " + reason);
            state.backoff = nextBackoff(state.backoff);
            state.next_attempt = std::chrono::steady_clock::now() + state.backoff;
            continue;
        }
        state.last_error.clear(); // the peer answered: an earlier "recv" or "http 500" is over
        auto const parsed = json::parse(response.body);
        if (parsed.is<picojson::object>())
        {
            auto const& obj = parsed.get<picojson::object>();
            state.replication_id = json::asString(obj, "replication_id").value_or(state.replication_id);
            state.state = json::asString(obj, "state").value_or("pending");
            state.source_error = state.state == "error";
            state.last_error = json::asString(obj, "error").value_or(""); // the source's reason (1.3.0)
            if (auto const head = obj.find("origin_head"); head != obj.end() && head->second.is<double>())
            {
                auto const value = static_cast<std::uint64_t>(head->second.get<double>());
                if (state.origin_head && *state.origin_head != value)
                {
                    state.origin_moved_at = std::chrono::steady_clock::now();
                }
                state.origin_head = value;
            }
        }
        if (state.state == "active")
        {
            state.backoff = std::chrono::milliseconds(250);
        }
    }
}

PostResult ReplicationEngine::post(PostRequest const& request, std::string const& domainPath)
{
    PostResult result;
    FabricEndpoint ep = endpoint(request.dest_host_id);
    if (!request.local_addr.empty())
    {
        ep.node = request.local_addr;
    }
    if (!request.provider.empty())
    {
        ep.provider = request.provider;
    }
    PostRequest stored = request;
    stored.local_addr = ep.node;
    stored.provider = ep.provider;
    {
        std::lock_guard const lock{mu_};
        result = handshake_.post(stored);
        // While the source's own fabric link to the destination is down it answers link_down with the
        // reason and sets nothing up. 1.3.0 tried, and without the address answered a bare 500.
        if (auto const down = linksDown_.find(request.dest_host_id); down != linksDown_.end())
        {
            result.state = "link_down";
            result.error = down->second;
            return result;
        }
    }
    std::shared_ptr<FabricDomain> dom;
    // A failure goes back with its reason, which the destination shows. A lane left empty goes.
    auto failed = [&](std::string const& error) {
        result.status = 500;
        result.state = "error";
        result.error = error;
        dom.reset();
        dropIfIdle(domainPath, request.flow_id);
        return result;
    };
    dom = domain(domainPath, request.flow_id);
    if (!dom || !dom->ok())
    {
        return failed(dom ? dom->error() : "no domain");
    }
    std::string error;
    if (!dom->ensureInitiator(result.replication_id, request.flow_id, ep, &error))
    {
        log::warn("initiator_failed", {{"flow_id", request.flow_id}, {"error", error}});
        return failed(error);
    }
    if (!dom->addInitiatorTarget(result.replication_id, request.dest_host_id, request.target_info, &error))
    {
        log::warn("add_target_failed", {{"flow_id", request.flow_id}, {"peer", request.dest_host_id}, {"error", error}});
        return failed(error);
    }
    result.origin_head = dom->originHead(result.replication_id);
    // The source's state and reason go back to the destination: "error" (a transfer that does not
    // complete) lets it set its target up again sooner, "link_down" makes it wait for the link.
    result.state = "pending";
    for (auto const& row : dom->rows())
    {
        if (row.key == result.replication_id)
        {
            if (row.state == "active" || row.state == "error")
            {
                result.state = row.state;
            }
            result.error = row.last_error;
        }
    }
    std::lock_guard const lock{mu_};
    if (result.state == "active")
    {
        handshake_.setTargetState(result.replication_id, request.dest_host_id, "active");
    }
    return result;
}

bool ReplicationEngine::eraseTarget(std::string const& replicationId, std::string const& destHostId)
{
    std::optional<SourceReplication> row;
    {
        std::lock_guard const lock{mu_};
        row = handshake_.find(replicationId);
        if (!row)
        {
            return false;
        }
    }
    // Only the lanes of that flow can hold its initiator.
    std::vector<std::shared_ptr<FabricDomain>> flowLanes;
    for (auto const& lane : lanes())
    {
        if (lane->flowId() == row->flow_id)
        {
            flowLanes.push_back(lane);
        }
    }
    for (auto const& lane : flowLanes)
    {
        lane->removeInitiatorTarget(replicationId, destHostId);
    }
    bool removed = false;
    bool last = false;
    {
        std::lock_guard const lock{mu_};
        removed = handshake_.eraseTarget(replicationId, destHostId);
        last = removed && !handshake_.find(replicationId);
    }
    std::vector<std::string> paths;
    for (auto const& lane : flowLanes)
    {
        if (last)
        {
            lane->destroyInitiator(replicationId);
        }
        paths.push_back(lane->path());
    }
    flowLanes.clear(); // dropIfIdle drops only a lane nobody else holds
    for (auto const& path : paths)
    {
        dropIfIdle(path, row->flow_id);
    }
    return removed;
}

std::vector<ReplicaView> ReplicationEngine::status() const
{
    std::map<std::string, std::string> linksDown;
    {
        std::lock_guard const lock{mu_};
        linksDown = linksDown_;
    }
    std::vector<ReplicaView> out;
    for (auto const& dom : lanes())
    {
        for (auto const& row : dom->rows())
        {
            ReplicaView view;
            view.replication_id = row.key;
            view.role = row.role;
            view.flow_id = row.flow_id;
            view.peer = row.peer;
            view.provider = row.provider;
            view.state = row.state;
            view.grains = row.grains;
            view.bytes = row.bytes;
            view.errors = row.errors;
            view.head = row.head;
            view.gaps = row.gaps;
            view.last_error = row.last_error;
            view.fallback = row.fallback;
            view.cq_depth = row.cq_depth;
            view.lag = static_cast<std::int64_t>(row.behind);
            view.lag_ms = row.behind_ms;
            if (auto const down = linksDown.find(row.peer); row.role == "source" && down != linksDown.end())
            {
                view.state = "link_down";
                view.last_error = down->second;
            }
            out.push_back(std::move(view));
        }
    }
    for (auto const& [key, state] : dest_)
    {
        bool found = false;
        for (auto& view : out)
        {
            if (view.replication_id == key && view.role == "destination")
            {
                view.state = state.state == "active" ? view.state : state.state;
                view.restarts = state.restarts;
                // Nobody writes the origin flow (no writer, or its head has not moved for 2 s): the
                // mirror is not behind, although the TAI index runs on.
                if (!state.source_active ||
                    (state.origin_head && std::chrono::steady_clock::now() - state.origin_moved_at > std::chrono::seconds(2)))
                {
                    view.lag = 0;
                    view.lag_ms = 0;
                }
                view.fallback = state.fallback || view.fallback;
                if (!state.provider.empty())
                {
                    view.provider = state.provider;
                }
                view.peer = key.substr(0, key.find('/'));
                if (!state.last_error.empty())
                {
                    view.last_error = state.last_error;
                }
                found = true;
            }
        }
        if (!found)
        {
            ReplicaView view;
            view.replication_id = key;
            view.role = "destination";
            view.peer = key.substr(0, key.find('/'));
            auto const first = key.find('/');
            auto const second = key.find('/', first + 1);
            if (second != std::string::npos)
            {
                view.domain_id = key.substr(first + 1, second - first - 1);
                view.flow_id = key.substr(second + 1);
            }
            view.state = state.state;
            view.restarts = state.restarts;
            view.last_error = state.last_error;
            view.provider = state.provider;
            view.fallback = state.fallback;
            out.push_back(std::move(view));
        }
    }
    return out;
}
} // namespace mfa
