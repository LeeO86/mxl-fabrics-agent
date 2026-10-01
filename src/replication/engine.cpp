#include "replication/engine.hpp"

#include "util/httpclient.hpp"
#include "util/jsonutil.hpp"
#include "util/logging.hpp"

#include <filesystem>

namespace mfa
{
namespace
{
std::string pullKey(PullRequest const& pull)
{
    return pull.peer + "/" + pull.domain_id + "/" + pull.flow_id;
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

std::shared_ptr<FabricDomain> ReplicationEngine::domain(std::string const& path)
{
    {
        std::lock_guard const lock{mu_};
        auto const it = domains_.find(path);
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
    auto created = std::make_shared<FabricDomain>(path, rt, cpus);
    if (!created->ok())
    {
        log::error("domain_fabric_failed", {{"path", path}, {"error", created->error()}});
    }
    std::lock_guard const lock{mu_};
    auto const it = domains_.find(path);
    if (it != domains_.end())
    {
        return it->second;
    }
    domains_[path] = created;
    return created;
}

void ReplicationEngine::releaseDomain(std::string const& path)
{
    std::lock_guard const lock{mu_};
    domains_.erase(path);
}

bool ReplicationEngine::ensureWriter(std::string const& path, std::string const& flowDef, std::string* error)
{
    auto dom = domain(path);
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
    std::shared_ptr<FabricDomain> dom;
    {
        std::lock_guard const lock{mu_};
        auto const it = domains_.find(path);
        if (it == domains_.end())
        {
            return;
        }
        dom = it->second;
    }
    dom->releaseWriter(flowId);
}

void ReplicationEngine::setPulls(std::vector<PullRequest> const& pulls)
{
    std::map<std::string, PullRequest> wanted;
    for (auto const& pull : pulls)
    {
        wanted[pullKey(pull)] = pull;
    }
    std::vector<std::pair<std::string, DestState>> drop;
    {
        std::lock_guard const lock{mu_};
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
            if (auto dom = domain(state.mirror_path))
            {
                dom->destroyTarget(key);
            }
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
            auto dom = domain(pull.mirror_path);
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
        if (std::chrono::steady_clock::now() < state.next_attempt && state.state == "error")
        {
            continue;
        }
        auto dom = domain(pull.mirror_path);
        if (!dom || !dom->ok())
        {
            state.state = "error";
            state.last_error = dom ? dom->error() : "domain";
            continue;
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
                state.state = "error";
                state.last_error = setup.error;
                state.backoff = std::min(state.backoff * 2, std::chrono::milliseconds(10000));
                state.next_attempt = std::chrono::steady_clock::now() + state.backoff;
                state.restarts += 1;
                log::warn("target_setup_failed", {{"flow_id", pull.flow_id}, {"peer", pull.peer}, {"error", setup.error}});
                continue;
            }
            state.target_info = setup.target_info;
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
            state.last_error = response.error.empty() ? ("http " + std::to_string(response.status)) : response.error;
            state.backoff = std::min(state.backoff * 2, std::chrono::milliseconds(10000));
            if (state.backoff.count() < 250)
            {
                state.backoff = std::chrono::milliseconds(250);
            }
            state.next_attempt = std::chrono::steady_clock::now() + state.backoff;
            continue;
        }
        auto const parsed = json::parse(response.body);
        if (parsed.is<picojson::object>())
        {
            auto const& obj = parsed.get<picojson::object>();
            state.replication_id = json::asString(obj, "replication_id").value_or(state.replication_id);
            state.state = json::asString(obj, "state").value_or("pending");
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
    }
    auto dom = domain(domainPath);
    if (!dom || !dom->ok())
    {
        result.status = 500;
        result.state = "error";
        return result;
    }
    std::string error;
    if (!dom->ensureInitiator(result.replication_id, request.flow_id, ep, &error))
    {
        result.status = 500;
        result.state = "error";
        log::warn("initiator_failed", {{"flow_id", request.flow_id}, {"error", error}});
        return result;
    }
    if (!dom->addInitiatorTarget(result.replication_id, request.dest_host_id, request.target_info, &error))
    {
        result.status = 500;
        result.state = "error";
        log::warn("add_target_failed", {{"flow_id", request.flow_id}, {"peer", request.dest_host_id}, {"error", error}});
        return result;
    }
    for (auto const& row : dom->rows())
    {
        if (row.key == result.replication_id && row.state == "active")
        {
            result.state = "active";
            std::lock_guard const lock{mu_};
            handshake_.setTargetState(result.replication_id, request.dest_host_id, "active");
            return result;
        }
    }
    result.state = "pending";
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
    for (auto const& [path, dom] : domains_)
    {
        dom->removeInitiatorTarget(replicationId, destHostId);
        auto const updated = handshake_.find(replicationId);
        if (updated && updated->targets.size() <= 1)
        {
            // erase below removes the last target and the row
        }
    }
    bool removed = false;
    {
        std::lock_guard const lock{mu_};
        removed = handshake_.eraseTarget(replicationId, destHostId);
        if (removed && !handshake_.find(replicationId))
        {
            for (auto const& [_, dom] : domains_)
            {
                dom->destroyInitiator(replicationId);
            }
        }
    }
    return removed;
}

std::vector<ReplicaView> ReplicationEngine::status() const
{
    std::vector<ReplicaView> out;
    std::map<std::string, std::uint64_t> sourceHead;
    for (auto const& [_, dom] : domains_)
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
            view.last_error = row.last_error;
            view.fallback = row.fallback;
            view.cq_depth = row.cq_depth;
            if (row.role == "source")
            {
                sourceHead[row.flow_id] = row.head;
            }
            out.push_back(std::move(view));
        }
    }
    for (auto& view : out)
    {
        if (view.role == "destination")
        {
            auto const it = sourceHead.find(view.flow_id);
            if (it != sourceHead.end() && it->second >= view.head)
            {
                view.lag = static_cast<std::int64_t>(it->second - view.head);
            }
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
