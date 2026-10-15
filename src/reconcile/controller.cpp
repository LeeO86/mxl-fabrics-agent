#include "reconcile/controller.hpp"

#include "mirror/lifecycle.hpp"
#include "replication/policy.hpp"
#include "util/httpclient.hpp"
#include "util/jsonutil.hpp"
#include "util/logging.hpp"
#include "util/net.hpp"
#include "util/uuid.hpp"
#include "version.hpp"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <limits>
#include <fstream>
#include <set>
#include <sstream>

namespace mfa
{
std::map<std::string, std::string> loadConfigFileText(std::string const& text);
}

#if __has_include("index_html.hpp")
#include "index_html.hpp"
#define MFA_HAS_UI 1
#endif

namespace mfa
{
namespace
{
namespace fs = std::filesystem;

std::string readText(fs::path const& path)
{
    std::ifstream in(path);
    if (!in)
    {
        return {};
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void writeText(fs::path const& path, std::string const& body)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << body;
}

std::string mirrorPath(std::string const& root, std::string const& domainId)
{
    return (fs::path(root) / ("mirror-" + domainId)).string();
}

std::string domainIdOf(std::string const& text)
{
    auto const root = json::parse(text);
    if (!root.is<picojson::object>())
    {
        return {};
    }
    return json::asString(root.get<picojson::object>(), "id").value_or("");
}

std::string domainDef(std::string const& domainId, std::string const& sourceHost, std::string const& owner)
{
    picojson::object marker;
    marker["mirror"] = picojson::value(true);
    marker["source_host_id"] = picojson::value(sourceHost);
    marker["owner_host_id"] = picojson::value(owner);
    picojson::object obj;
    obj["id"] = picojson::value(domainId);
    obj["label"] = picojson::value("mirror " + domainId);
    // BCP-007-03 requires description and tags as well.
    obj["description"] = picojson::value("Mirror of MXL domain " + domainId + " from " + sourceHost + ", written by mxl-fabrics-agent");
    obj["tags"] = picojson::value(picojson::object{});
    obj["x-mxl-fabrics-agent"] = picojson::value(marker);
    return picojson::value(obj).serialize(true);
}

bool contains(std::vector<std::string> const& items, std::string const& id)
{
    return std::find(items.begin(), items.end(), id) != items.end();
}

FlowFilter makeFilter(Config const& cfg, std::string const& domainId, std::string const& flowId)
{
    FlowFilter filter;
    filter.includeDomainsEmpty = cfg.mirror_include_domains.empty();
    filter.includeFlowsEmpty = cfg.mirror_include_flows.empty();
    filter.domainIncluded = contains(cfg.mirror_include_domains, domainId);
    filter.flowIncluded = contains(cfg.mirror_include_flows, flowId);
    filter.domainExcluded = contains(cfg.mirror_exclude_domains, domainId);
    filter.flowExcluded = contains(cfg.mirror_exclude_flows, flowId);
    return filter;
}

std::uint64_t freeMb(std::string const& path)
{
    struct statvfs st{};
    if (statvfs(path.c_str(), &st) != 0)
    {
        return 0;
    }
    return static_cast<std::uint64_t>(st.f_bavail) * static_cast<std::uint64_t>(st.f_frsize) / (1024ULL * 1024ULL);
}

std::string testFlow(std::string const& id)
{
    return std::string(R"({
  "id": ")") + id +
           R"(",
  "label": "fabrics agent link test",
  "description": "temporary flow",
  "format": "urn:x-nmos:format:data",
  "media_type": "video/smpte291",
  "grain_rate": {"numerator": 25, "denominator": 1},
  "tags": {"urn:x-nmos:tag:grouphint/v1.0": ["mxl-fabrics-agent:Data"]}
})";
}

const char kFallbackUi[] = R"HTML(<!doctype html><html><head><meta charset="utf-8"><title>mxl-fabrics-agent</title></head>
<body><h1>mxl-fabrics-agent</h1><p>The admin UI was not embedded in this build. API is at <a href="/api/v1/info">/api/v1/info</a>.</p></body></html>)HTML";

double taiNowSeconds()
{
    timespec ts{};
    ::clock_gettime(CLOCK_TAI, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

std::int64_t unixNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The mirror flow's data file: its inode and head index (none before the first grain).
void readMirrorFile(std::string const& domainPath, std::string const& flowId, MirrorObservation& obs)
{
    auto const header = readFlowHeader((fs::path(domainPath) / (flowId + ".mxl-flow") / "data").string());
    obs.inode = header.inode;
    if (header.headIndex != 0 && header.headIndex != std::numeric_limits<std::uint64_t>::max())
    {
        obs.head = header.headIndex;
    }
}

picojson::value unixSeconds(std::int64_t unixMs)
{
    return unixMs > 0 ? picojson::value(static_cast<double>(unixMs) / 1000.0) : picojson::value();
}

void putHealth(picojson::object& obj, MirrorHealth const& h, int staleMs)
{
    obj["state"] = picojson::value(h.state);
    obj["reason"] = picojson::value(h.reason);
    obj["last_grain_age_ms"] =
        h.last_grain_age_ms ? picojson::value(std::round(*h.last_grain_age_ms * 1000.0) / 1000.0) : picojson::value();
    obj["stale_since"] = unixSeconds(h.stale_since_unix_ms);
    obj["stale_after_ms"] = picojson::value(static_cast<double>(staleMs));
    obj["rebuilds"] = picojson::value(static_cast<double>(h.rebuilds));
    obj["last_rebuild"] = unixSeconds(h.last_rebuild_unix_ms);
    obj["last_rebuild_reason"] = h.last_rebuild_reason.empty() ? picojson::value() : picojson::value(h.last_rebuild_reason);
}

LocalIdentity identityFrom(Config const& cfg)
{
    LocalIdentity id;
    id.hostname = localHostname();
    id.addresses = localAddresses();
    id.extraNodeIds.insert(cfg.local_node_ids.begin(), cfg.local_node_ids.end());
    id.extraHostnames.insert(cfg.local_node_hostnames.begin(), cfg.local_node_hostnames.end());
    id.cidrs = cfg.local_node_cidrs;
    if (!id.cidrs.empty())
    {
        id.resolveName = [](std::string const& name) { return resolveNameCached(name); };
    }
    id.ownNodeId = nmosNodeId(cfg.nmos_seed, cfg.host_id);
    return id;
}
} // namespace

Controller::Controller(std::shared_ptr<ConfigStore> store)
    : store_(std::move(store))
    , bootId_(uuidV4().str())
    , engine_(store_->get())
    , scanner_(store_->get().mxl_root, store_->get().host_id, store_->get().scan_interval_ms, [this] { cv_.notify_all(); })
    , peers_(store_->get(), [this] { cv_.notify_all(); })
    , observer_(store_->get(), identityFrom(store_->get()), [this] { cv_.notify_all(); })
{
    tai_ = taiOffsetSeconds();
    rootTmpfs_ = pathIsTmpfs(store_->get().mxl_root);
    // Environment only, read once: neither the config file nor PUT /api/v1/config can open the rebuild hook.
    adminRebuild_ = store_->get().mirror_admin_rebuild;
    // Source grains, from the transfer to its completion.
    engine_.setTransferObserver([this](std::string const& provider, double seconds) {
        metrics_.observe("grain_transfer_seconds", seconds, {{"provider", provider}});
    });
}

Controller::~Controller()
{
    stop();
}

void Controller::start()
{
    started_ = std::chrono::steady_clock::now();
    auto const cfg = store_->get();
    if (cfg.nmos_enable)
    {
        node_ = std::make_unique<NmosNode>(
            cfg, nmosNodeId(cfg.nmos_seed, cfg.host_id),
            [this](bool ok, std::string const& error) {
                nmosReady_ = ok;
                nmosError_ = error;
                if (!ok && error.find("built without") == std::string::npos)
                {
                    fatal_ = 75;
                }
                cv_.notify_all();
            },
            [this](bool registered) {
                nmosRegistered_ = registered;
                cv_.notify_all();
            });
    }
    // The scanner first: the HTTP server already answers, and /api/v1/inventory waits for the first scan.
    scanner_.start();
    if (node_)
    {
        node_->start();
    }
    peers_.start();
    observer_.start();
    stop_ = false;
    thread_ = std::thread([this] { loop(); });
}

void Controller::stop()
{
    {
        std::lock_guard const lock{mu_};
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
    {
        thread_.join();
    }
    auto const cfg = store_->get();
    // Without cleanup the mirror flows stay for the next start, so readers on this host keep them.
    engine_.releaseAll(!cfg.cleanup_mirrors_on_exit);
    if (node_)
    {
        node_->stop();
    }
    if (cfg.cleanup_mirrors_on_exit)
    {
        std::error_code ec;
        for (auto const& entry : fs::directory_iterator(cfg.mxl_root, ec))
        {
            auto const def = readText(entry.path() / "domain_def.json");
            auto const marker = readMirrorMarker(def);
            if (marker && marker->mirror && marker->owner_host_id == cfg.host_id)
            {
                fs::remove_all(entry.path(), ec);
                log::info("mirror_domain_removed", {{"path", entry.path().string()}});
            }
        }
    }
    observer_.stop();
    peers_.stop();
    scanner_.stop();
    {
        std::lock_guard const lock{eventMu_};
        eventsStop_ = true;
    }
    eventCv_.notify_all();
}

void Controller::publish(std::string const& event, std::string const& data)
{
    {
        std::lock_guard const lock{eventMu_};
        events_.push_back(Event{event, data});
        if (events_.size() > 200)
        {
            events_.erase(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(events_.size() - 200));
        }
    }
    eventCv_.notify_all();
}

void Controller::loop()
{
    while (true)
    {
        {
            std::unique_lock lock{mu_};
            if (stop_)
            {
                return;
            }
            cv_.wait_for(lock, std::chrono::milliseconds(200), [&] { return stop_; });
            if (stop_)
            {
                return;
            }
        }
        try
        {
            tick();
            ready_ = true;
        }
        catch (std::exception const& ex)
        {
            log::error("reconcile_failed", {{"error", ex.what()}});
        }
    }
}

void Controller::tick()
{
    std::lock_guard const tickLock{tickMu_};
    auto const cfg = store_->get();
    engine_.updateConfig(cfg);
    peers_.updateConfig(cfg);
    observer_.updateConfig(cfg);
    auto const nmos = observer_.snapshot();
    std::vector<PeerSnapshot> discovered;
    for (auto const& agent : nmos.agents)
    {
        if (agent.host_id != cfg.host_id)
        {
            discovered.push_back(agent);
        }
    }
    peers_.setDiscovered(discovered);
    auto const peerSnap = peers_.snapshot();
    std::set<std::string> peerDomains;
    struct Remote
    {
        PeerSnapshot const* peer;
        DomainRecord const* domain;
        FlowRecord const* flow;
    };
    std::vector<Remote> remotes;
    for (auto const& peer : peerSnap)
    {
        if (!peer.up)
        {
            continue;
        }
        for (auto const& domain : peer.inventory.domains)
        {
            peerDomains.insert(domain.domain_id);
            for (auto const& flow : domain.flows)
            {
                remotes.push_back(Remote{&peer, &domain, &flow});
            }
        }
    }
    // The fabric link to each peer: carrier of the local interface of its fabric address. One log line
    // when it goes down or comes back.
    std::map<std::string, FabricLink> links;
    std::map<std::string, std::string> linksDown;
    for (auto const& peer : peerSnap)
    {
        auto const link = fabricLink(peer.local_addr.empty() ? cfg.fabric_interface : peer.local_addr);
        auto& logged = linkErrors_[peer.host_id];
        if (link.error != logged)
        {
            if (link.up)
            {
                log::info("peer_link_up", {{"peer", peer.host_id}, {"interface", link.netdev}});
            }
            else
            {
                log::warn("peer_link_down", {{"peer", peer.host_id}, {"interface", link.netdev}, {"reason", link.error}});
            }
            logged = link.error;
        }
        if (!link.up)
        {
            linksDown[peer.host_id] = link.error;
        }
        links[peer.host_id] = link;
    }
    engine_.setLinksDown(linksDown);
    // Peers that hold the same flow (a function moved to another host and left its domain behind):
    // replicate from one, preferring the origin that is being written. Logged when it starts or changes.
    std::map<std::string, std::vector<OriginCandidate>> holders;
    for (auto const& remote : remotes)
    {
        holders[remote.domain->domain_id + "/" + remote.flow->flow_id].push_back(
            OriginCandidate{remote.peer->host_id, remote.flow->active, remote.flow->live});
    }
    std::map<std::string, OriginConflict> conflicts;
    for (auto const& [key, candidates] : holders)
    {
        if (candidates.size() < 2)
        {
            continue;
        }
        auto const previous = originConflicts_.find(key);
        OriginConflict conflict;
        for (auto const& candidate : candidates)
        {
            conflict.hosts += (conflict.hosts.empty() ? "" : ",") + candidate.host + (candidate.live ? "(live)" : candidate.active ? "(writer)" : "");
        }
        conflict.chosen = chooseOrigin(candidates, previous != originConflicts_.end() ? previous->second.chosen : std::string());
        if (previous == originConflicts_.end() || previous->second.hosts != conflict.hosts || previous->second.chosen != conflict.chosen)
        {
            log::warn("origin_conflict", {{"flow", key}, {"hosts", conflict.hosts}, {"chosen", conflict.chosen}});
        }
        conflicts[key] = conflict;
    }
    originConflicts_ = conflicts;
    auto const inventory = scanner_.snapshot();
    std::set<std::string> localIds;
    for (auto const& domain : inventory.domains)
    {
        if (domain.kind == "local")
        {
            localIds.insert(domain.domain_id);
        }
    }
    std::set<std::string> flowKeys;
    for (auto const& remote : remotes)
    {
        flowKeys.insert(remote.domain->domain_id + "/" + remote.flow->flow_id);
    }
    auto demand = deriveDemand(nmos.receivers, localIds, peerDomains, flowKeys);
    for (auto& entry : demand.entries)
    {
        for (auto const& peer : peerSnap)
        {
            for (auto const& domain : peer.inventory.domains)
            {
                if (domain.domain_id == entry.domain_id)
                {
                    entry.source_host = peer.host_id;
                }
            }
        }
        if (auto const conflict = conflicts.find(entry.domain_id + "/" + entry.flow_id); conflict != conflicts.end())
        {
            entry.source_host = conflict->second.chosen;
        }
    }
    std::set<std::string> rawDemand;
    for (auto const& entry : demand.entries)
    {
        if (entry.state != "stale_reference")
        {
            rawDemand.insert(entry.domain_id + "/" + entry.flow_id);
        }
    }
    auto const now = std::chrono::steady_clock::now();
    std::set<std::string> seen;
    std::vector<PullRequest> pulls;
    std::vector<DomainRecord> mirrorViews;
    std::set<std::string> wantedDomains; // mirror domains with demand (a receiver, a held replication, eager mode)
    auto const free = freeMb(cfg.mxl_root);
    std::vector<MirrorObservation> remoteObservations;
    for (auto const& remote : remotes)
    {
        auto const key = remote.domain->domain_id + "/" + remote.flow->flow_id;
        if (auto const conflict = conflicts.find(key); conflict != conflicts.end() && conflict->second.chosen != remote.peer->host_id)
        {
            continue;
        }
        seen.insert(key);
        bool const originLive = remote.peer->up && remote.flow->active;
        if (!originLive)
        {
            if (originLost_.count(key) == 0)
            {
                originLost_[key] = now;
            }
        }
        else
        {
            originLost_.erase(key);
        }
        bool const wanted = rawDemand.count(key) != 0;
        if (wanted)
        {
            demandLost_.erase(key);
        }
        else if (lastDemand_.count(key) != 0 && demandLost_.count(key) == 0)
        {
            demandLost_[key] = now;
        }
        else if (lastDemand_.count(key) == 0)
        {
            demandLost_.erase(key);
        }
        bool const graceHolding = originLost_.count(key) != 0 &&
                                   now - originLost_[key] < std::chrono::seconds(cfg.mirror_grace_s);
        bool const graceExpired = originLost_.count(key) != 0 && !graceHolding;
        bool const hold = wanted || (demandLost_.count(key) != 0 && now - demandLost_[key] < std::chrono::milliseconds(cfg.release_grace_ms));
        auto const path = mirrorPath(cfg.mxl_root, remote.domain->domain_id);
        bool exists = false;
        {
            std::error_code ec;
            exists = fs::is_directory(fs::path(path) / (remote.flow->flow_id + ".mxl-flow"), ec);
        }
        // An origin created again with another definition (rate, format, geometry) under the same flow id:
        // the mirror keeps the layout it was created with, so it is rebuilt from the new definition. With the
        // same definition the mirror stays and grains go on in the same file.
        if (exists && localIds.count(remote.domain->domain_id) == 0 &&
            flowDefinitionChanged(readText(fs::path(path) / (remote.flow->flow_id + ".mxl-flow") / "flow_def.json"),
                remote.flow->flow_def_json))
        {
            auto const last = configRebuiltAt_.find(key);
            if (last == configRebuiltAt_.end() || now - last->second >= std::chrono::seconds(5))
            {
                configRebuiltAt_[key] = now;
                log::warn("mirror_origin_changed", {{"domain_id", remote.domain->domain_id}, {"flow_id", remote.flow->flow_id},
                                                       {"peer", remote.peer->host_id}});
                teardownMirrorFlow(path, remote.domain->domain_id, remote.flow->flow_id, "origin_config_changed");
                std::error_code ec;
                exists = fs::is_directory(fs::path(path) / (remote.flow->flow_id + ".mxl-flow"), ec);
            }
        }
        auto const estimateMb = std::max<std::uint64_t>(1, remote.flow->payload_size * std::max<std::uint64_t>(remote.flow->ring_depth, 1) / (1024ULL * 1024ULL));
        MirrorInput input;
        input.exists = exists;
        input.origin_present = true;
        input.origin_active = remote.flow->active;
        input.peer_up = remote.peer->up;
        input.eager = cfg.mirror_mode == "eager";
        input.included = flowIncluded(makeFilter(cfg, remote.domain->domain_id, remote.flow->flow_id));
        input.conflict = localIds.count(remote.domain->domain_id) != 0;
        input.space_ok = free > static_cast<std::uint64_t>(cfg.tmpfs_reserve_mb) + (exists ? 0 : estimateMb);
        input.raw_demand = wanted;
        input.hold_replication = hold && remote.flow->active && remote.peer->up;
        input.grace_holding = graceHolding;
        input.grace_expired = graceExpired;
        auto const plan = planMirror(input);
        if (plan.create || wanted || input.hold_replication)
        {
            wantedDomains.insert(remote.domain->domain_id);
        }
        if (plan.remove && exists)
        {
            auto const reason = !input.included ? "excluded" : !remote.flow->active ? "origin_gone" : "no_demand";
            mirrorHealth_.noteRemoved(key, reason);
            engine_.releaseWriter(path, remote.flow->flow_id);
            std::error_code ec;
            fs::remove_all(fs::path(path) / (remote.flow->flow_id + ".mxl-flow"), ec);
            metrics_.addCounter("mirror_flows_removed_total", 1, {{"reason", reason}});
            log::info("mirror_removed", {{"flow_id", remote.flow->flow_id}, {"domain_id", remote.domain->domain_id}});
        }
        if (plan.create)
        {
            auto const defPath = fs::path(path) / "domain_def.json";
            auto const existing = readText(defPath);
            bool const mismatch = !existing.empty() && domainIdOf(existing) != remote.domain->domain_id;
            if (mismatch)
            {
                log::error("mirror_domain_id_mismatch",
                    {{"path", defPath.string()}, {"existing", domainIdOf(existing)}, {"wanted", remote.domain->domain_id}});
            }
            else
            {
                if (existing.empty())
                {
                    writeText(defPath, domainDef(remote.domain->domain_id, remote.peer->host_id, cfg.host_id));
                }
                if (!remote.domain->options_json.empty())
                {
                    auto const optionsPath = fs::path(path) / "options.json";
                    if (!fs::exists(optionsPath))
                    {
                        writeText(optionsPath, remote.domain->options_json);
                    }
                }
                std::string error;
                if (!remote.flow->flow_def_json.empty())
                {
                    engine_.ensureWriter(path, remote.flow->flow_def_json, &error);
                }
                if (!error.empty())
                {
                    log::warn("mirror_writer_failed", {{"flow_id", remote.flow->flow_id}, {"error", error}});
                }
            }
        }
        DomainRecord view;
        view.domain_id = remote.domain->domain_id;
        view.path = path;
        view.kind = "mirror";
        view.source_host_id = remote.peer->host_id;
        view.owner_host_id = cfg.host_id;
        FlowRecord flow = *remote.flow;
        flow.active = plan.create || exists;
        view.flows.push_back(flow);
        view.options_json = plan.state;
        mirrorViews.push_back(view);
        if (plan.replicate && !remote.flow->flow_def_json.empty())
        {
            PullRequest pull;
            pull.domain_id = remote.domain->domain_id;
            pull.flow_id = remote.flow->flow_id;
            pull.flow_def_json = remote.flow->flow_def_json;
            pull.mirror_path = path;
            pull.peer = remote.peer->host_id;
            pull.control_url = remote.peer->control_url;
            pull.local_addr = remote.peer->local_addr.empty() ? cfg.fabric_interface : remote.peer->local_addr;
            pull.provider = remote.peer->provider.empty() ? cfg.default_provider : remote.peer->provider;
            pull.allow_tcp_fallback = cfg.provider_fallback == "tcp";
            pull.peer_boot = remote.peer->boot_id;
            pull.peer_revision = remote.peer->revision;
            pull.source_active = remote.flow->active;
            pulls.push_back(std::move(pull));
        }
        // Health of the mirror flow: grains are due while a replication is wanted and the origin is live.
        MirrorObservation obs;
        obs.domain_id = remote.domain->domain_id;
        obs.flow_id = remote.flow->flow_id;
        obs.peer = remote.peer->host_id;
        obs.rate_num = remote.flow->grain_rate_num;
        obs.rate_den = remote.flow->grain_rate_den;
        obs.origin_head = remote.flow->head_index;
        obs.origin_live = remote.flow->live;
        obs.expected = plan.replicate && remote.flow->live;
        obs.idle_reason = !remote.flow->active ? "origin_inactive" : !plan.replicate ? "no_demand" : "origin_not_live";
        if (auto const link = links.find(remote.peer->host_id); link != links.end() && !link->second.up)
        {
            obs.blocked = "link_down";
        }
        remoteObservations.push_back(std::move(obs));
    }
    // The throttle of rebuilds for a changed origin definition only needs the last 5 s.
    for (auto it = configRebuiltAt_.begin(); it != configRebuiltAt_.end();)
    {
        it = now - it->second >= std::chrono::seconds(5) ? configRebuiltAt_.erase(it) : std::next(it);
    }
    for (auto it = originLost_.begin(); it != originLost_.end();)
    {
        if (seen.count(it->first) == 0 && now - it->second > std::chrono::seconds(cfg.mirror_grace_s))
        {
            it = originLost_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    lastDemand_ = std::move(rawDemand);
    engine_.setPulls(pulls);
    // Whole mirror domains, after setPulls has dropped the targets they no longer need. Orphans only
    // once the peers are known: 30 s after start and, with NMOS, while the registry answers.
    bool const truthOk = now - started_ >= std::chrono::seconds(30) && (!cfg.nmos_enable || nmos.registry_up);
    // What an up peer does not list counts only once every up peer's inventory is settled: read on two polls
    // in a row of the same process and from a finished scan. A peer that has just started (or this agent)
    // may hold an inventory without the flows yet (1.7.1).
    bool const inventoriesSettled =
        std::all_of(peerSnap.begin(), peerSnap.end(), [](PeerSnapshot const& peer) { return !peer.up || inventorySettled(peer); });
    auto domainViews = cleanupMirrors(cfg, inventory, peerSnap, truthOk, inventoriesSettled, localIds, peerDomains, wantedDomains, mirrorViews);
    // Then single flows whose origin is gone in the domains that stay; the health below shows them absent.
    cleanupMirrorFlows(cfg, inventory, peerSnap, truthOk, inventoriesSettled, seen);
    observeMirrors(cfg, inventory, peerSnap, std::move(remoteObservations), seen, inventoriesSettled);
    {
        std::lock_guard const lock{mu_};
        mirrorHealthView_ = mirrorHealth_.entries();
        mirrorDomains_ = std::move(domainViews);
        demand_ = std::move(demand);
        mirrors_ = std::move(mirrorViews);
        links_ = std::move(links);
    }
    renderMetrics();
    publish("inventory", std::to_string(inventory.revision));
}

std::vector<Controller::MirrorDomainView> Controller::cleanupMirrors(Config const& cfg, Inventory const& inventory,
    std::vector<PeerSnapshot> const& peerSnap, bool truthOk, bool inventoriesSettled, std::set<std::string> const& localIds,
    std::set<std::string> const& peerDomains, std::set<std::string> const& wanted, std::vector<DomainRecord> const& flowViews)
{
    auto const now = std::chrono::steady_clock::now();
    auto const wallNow = std::chrono::system_clock::now();
    std::map<std::string, MirrorDomainView> views;
    for (auto const& domain : inventory.domains)
    {
        // Only our own mirror directories (mirror-<domain id> with our marker): never an origin, a
        // mirror of another agent or anything else below the root.
        if (domain.kind != "mirror" || fs::path(domain.path).filename().string() != "mirror-" + domain.domain_id)
        {
            continue;
        }
        auto& view = views[domain.domain_id];
        view.domain_id = domain.domain_id;
        view.path = domain.path;
        view.source_host_id = domain.source_host_id;
        view.on_disk = true;
        struct stat st{};
        if (::stat((fs::path(domain.path) / "domain_def.json").c_str(), &st) == 0)
        {
            view.created_unix = static_cast<std::int64_t>(st.st_mtime);
        }
        for (auto const& flow : domain.flows)
        {
            view.flows.emplace_back(flow, "");
        }
    }
    for (auto const& flowView : flowViews)
    {
        auto& view = views[flowView.domain_id];
        view.domain_id = flowView.domain_id;
        if (view.path.empty())
        {
            view.path = flowView.path;
        }
        if (view.source_host_id.empty())
        {
            view.source_host_id = flowView.source_host_id;
        }
        for (auto const& flow : flowView.flows)
        {
            auto const it = std::find_if(view.flows.begin(), view.flows.end(), [&](auto const& f) { return f.first.flow_id == flow.flow_id; });
            if (it == view.flows.end())
            {
                view.flows.emplace_back(flow, flowView.options_json);
            }
            else
            {
                it->second = flowView.options_json;
            }
        }
    }
    std::vector<std::string> candidates;
    std::map<std::string, MirrorDomainPlan> plans;
    for (auto& [id, view] : views)
    {
        view.origin_local = localIds.count(id) != 0;
        view.origin_on_peer = peerDomains.count(id) != 0;
        bool sourceKnownDown = false;
        for (auto const& peer : peerSnap)
        {
            if (peer.host_id == view.source_host_id)
            {
                view.source_up = peer.up;
                sourceKnownDown = !peer.up;
            }
        }
        view.demand = wanted.count(id) != 0;
        if (view.demand)
        {
            mirrorIdleSince_.erase(id);
        }
        else
        {
            auto const since = mirrorIdleSince_.try_emplace(id, now).first->second;
            view.idle_since_unix = std::chrono::duration_cast<std::chrono::seconds>(
                (wallNow - std::chrono::duration_cast<std::chrono::system_clock::duration>(now - since)).time_since_epoch())
                                       .count();
        }
        MirrorDomainInput input;
        input.origin_local = view.origin_local;
        input.origin_on_peer = view.origin_on_peer || !truthOk || !inventoriesSettled; // not an orphan until the peers are known
        input.source_peer_down = sourceKnownDown;
        input.demand = view.demand;
        input.idle_expired = !view.demand && cfg.mirror_idle_grace_s > 0 &&
                             now - mirrorIdleSince_[id] >= std::chrono::seconds(cfg.mirror_idle_grace_s);
        auto const plan = planMirrorDomain(input);
        view.state = plan.state;
        // The scanner's snapshot may still list a domain that DELETE removed meanwhile.
        if (view.on_disk && plan.remove && now - started_ >= std::chrono::seconds(30) && fs::is_directory(view.path))
        {
            candidates.push_back(id);
            plans[id] = plan;
        }
    }
    for (auto it = mirrorIdleSince_.begin(); it != mirrorIdleSince_.end();)
    {
        it = views.count(it->first) == 0 ? mirrorIdleSince_.erase(it) : std::next(it);
    }
    for (auto it = mirrorLogged_.begin(); it != mirrorLogged_.end();)
    {
        it = views.count(it->first) == 0 ? mirrorLogged_.erase(it) : std::next(it);
    }
    // Removal candidates: check the readers (at most every 2 s), then remove what no other process maps.
    if (!candidates.empty() && now >= nextReaderScan_)
    {
        nextReaderScan_ = now + std::chrono::seconds(2);
        for (auto const& id : candidates)
        {
            auto const& view = views[id];
            auto const reason = plans[id].reason;
            if (cfg.mirror_cleanup_dry_run)
            {
                if (mirrorLogged_[id] != "dry_run:" + reason)
                {
                    log::info("mirror_domain_would_remove", {{"domain_id", id}, {"path", view.path}, {"reason", reason}});
                    mirrorLogged_[id] = "dry_run:" + reason;
                }
                continue;
            }
            std::vector<ReaderProcess> readers;
            auto const refused = removeMirrorDomain(id, view.path, reason, &readers);
            if (refused.empty())
            {
                mirrorIdleSince_.erase(id);
                mirrorLogged_.erase(id);
                metrics_.addCounter("mirror_domains_removed_total", 1, {{"reason", reason}});
                continue;
            }
            if (mirrorLogged_[id] != "refused:" + reason + ":" + refused)
            {
                log::warn("mirror_domain_busy", {{"domain_id", id}, {"path", view.path}, {"reason", reason}, {"refused", refused}});
                metrics_.addCounter("mirror_domains_refused_total", 1, {{"reason", reason}});
                mirrorLogged_[id] = "refused:" + reason + ":" + refused;
            }
        }
    }
    std::vector<MirrorDomainView> result;
    for (auto& [id, view] : views)
    {
        result.push_back(std::move(view));
    }
    return result;
}

void Controller::teardownMirrorFlow(std::string const& path, std::string const& domainId, std::string const& flowId,
    std::string const& reason)
{
    mirrorHealth_.noteRemoved(domainId + "/" + flowId, reason);
    engine_.resetMirrorFlow(path, domainId, flowId);
    std::error_code ec;
    fs::remove_all(fs::path(path) / (flowId + ".mxl-flow"), ec);
    if (ec)
    {
        log::warn("mirror_flow_remove_failed", {{"domain_id", domainId}, {"flow_id", flowId}, {"path", path}, {"error", ec.message()}});
    }
}

HttpResponse Controller::rebuildMirror(MirrorRebuildRequest const& request)
{
    auto const& domainId = request.domain_id;
    auto const& flowId = request.flow_id;
    auto const json = [](int status, picojson::object const& body) {
        return HttpResponse{status, "application/json", picojson::value(body).serialize(), false};
    };
    picojson::object body;
    body["domain_id"] = picojson::value(domainId);
    body["flow_id"] = picojson::value(flowId);
    if (auto const refused = mirrorRebuildRefusal(adminRebuild_, request, true); refused != 0)
    {
        body["error"] = picojson::value(refused == 403 ? "rebuild disabled: start the agent with MIRROR_ADMIN_REBUILD=true"
                                                       : "domain_id and flow_id must be UUIDs");
        return json(refused, body);
    }
    std::lock_guard const tickLock{tickMu_};
    // Only a flow of this agent's own mirror domains, as for DELETE /api/v1/mirrors/{domain_id}.
    std::string path;
    for (auto const& domain : scanner_.snapshot().domains)
    {
        if (domain.domain_id == domainId && domain.kind == "mirror" && fs::path(domain.path).filename().string() == "mirror-" + domainId)
        {
            path = domain.path;
        }
    }
    auto const flowDir = fs::path(path) / (flowId + ".mxl-flow");
    auto const def = path.empty() ? std::string() : readText(flowDir / "flow_def.json");
    if (mirrorRebuildRefusal(adminRebuild_, request, !path.empty() && !def.empty()) == 404)
    {
        body["error"] = picojson::value("no mirror flow of this agent with these ids");
        return json(404, body);
    }
    // The processes that map the flow now: they get MXL_ERR_FLOW_INVALID and have to open it again.
    std::map<FileKey, std::string> files;
    collectFiles(flowDir.string(), flowDir.string(), files);
    auto const scan = scanReaders(files, "/proc", static_cast<int>(::getpid()));
    std::vector<ReaderProcess> readers;
    removalBlocker(scan, flowDir.string(), &readers);
    auto const oldInode = fileInode((flowDir / "data").string());
    teardownMirrorFlow(path, domainId, flowId, "api");
    std::string error;
    engine_.ensureWriter(path, def, &error);
    auto const inode = fileInode((flowDir / "data").string());
    log::warn("mirror_rebuild_requested", {{"domain_id", domainId}, {"flow_id", flowId}, {"old_inode", std::to_string(oldInode)},
                                              {"inode", std::to_string(inode)}, {"readers", std::to_string(readers.size())},
                                              {"error", error}});
    body["old_inode"] = picojson::value(static_cast<double>(oldInode));
    body["inode"] = picojson::value(static_cast<double>(inode));
    picojson::array pids;
    for (auto const& reader : readers)
    {
        picojson::object r;
        r["pid"] = picojson::value(static_cast<double>(reader.pid));
        r["comm"] = picojson::value(reader.comm);
        pids.push_back(picojson::value(r));
    }
    body["readers"] = picojson::value(pids);
    if (inode == 0)
    {
        // Torn down, not created again: the next reconcile pass creates it when the mirror is still wanted.
        body["rebuilt"] = picojson::value(false);
        body["error"] = picojson::value(error.empty() ? std::string("mirror flow not created again") : error);
        return json(500, body);
    }
    body["rebuilt"] = picojson::value(true);
    return json(200, body);
}

std::string Controller::removeMirrorDomain(std::string const& domainId, std::string const& path, std::string const& reason,
    std::vector<ReaderProcess>* readers)
{
    auto const check = [&]() -> std::string {
        std::map<FileKey, std::string> files;
        collectFiles(path, path, files);
        return removalBlocker(scanReaders(files, "/proc", static_cast<int>(::getpid())), path, readers);
    };
    if (auto const busy = check(); !busy.empty())
    {
        return busy;
    }
    // The agent's own writers and targets first, then a last reader check right before the files go.
    std::vector<std::string> flows;
    std::error_code ec;
    for (auto const& entry : fs::directory_iterator(path, ec))
    {
        auto const name = entry.path().filename().string();
        if (name.size() > 9 && name.substr(name.size() - 9) == ".mxl-flow")
        {
            flows.push_back(name.substr(0, name.size() - 9));
        }
    }
    for (auto const& flowId : flows)
    {
        engine_.releaseWriter(path, flowId);
    }
    engine_.releaseDomain(path);
    if (auto const busy = check(); !busy.empty())
    {
        return busy;
    }
    std::string removed;
    for (auto const& flowId : flows)
    {
        fs::remove_all(fs::path(path) / (flowId + ".mxl-flow"), ec);
        removed += (removed.empty() ? "" : ",") + flowId;
    }
    fs::remove_all(path, ec);
    if (ec)
    {
        log::warn("mirror_domain_remove_failed", {{"domain_id", domainId}, {"path", path}, {"error", ec.message()}});
        return "remove failed: " + ec.message();
    }
    for (auto const& flowId : flows)
    {
        mirrorHealth_.noteRemoved(domainId + "/" + flowId, "mirror_domain_" + reason);
    }
    log::info("mirror_domain_removed", {{"domain_id", domainId}, {"path", path}, {"reason", reason}, {"flows", removed}});
    return {};
}

void Controller::cleanupMirrorFlows(Config const& cfg, Inventory const& inventory, std::vector<PeerSnapshot> const& peerSnap,
    bool truthOk, bool inventoriesSettled, std::set<std::string> const& seen)
{
    auto const now = std::chrono::steady_clock::now();
    struct Candidate
    {
        std::string key;
        std::string domain_id;
        std::string flow_id;
        std::string domain_path;
        std::string flow_path;
        std::string reason;
        std::int64_t missing_s = 0;
    };
    std::vector<Candidate> candidates;
    std::set<std::string> missing;
    for (auto const& domain : inventory.domains)
    {
        // Only our own mirror directories, as for whole domains: never an origin or another agent's mirror.
        if (domain.kind != "mirror" || fs::path(domain.path).filename().string() != "mirror-" + domain.domain_id)
        {
            continue;
        }
        bool sourceUp = false;
        for (auto const& peer : peerSnap)
        {
            if (peer.host_id == domain.source_host_id)
            {
                sourceUp = peer.up;
            }
        }
        for (auto const& flow : domain.flows)
        {
            auto const key = domain.domain_id + "/" + flow.flow_id;
            MirrorFlowInput input;
            input.offered = seen.count(key) != 0;
            input.source_up = sourceUp;
            input.peers_known = truthOk;
            input.inventories_settled = inventoriesSettled;
            std::chrono::steady_clock::time_point since = now;
            if (!input.offered && input.source_up && input.inventories_settled)
            {
                since = flowMissingSince_.try_emplace(key, now).first->second;
                input.grace_expired = cfg.mirror_idle_grace_s > 0 && now - since >= std::chrono::seconds(cfg.mirror_idle_grace_s);
            }
            auto const plan = planMirrorFlow(input);
            if (!plan.missing)
            {
                continue;
            }
            missing.insert(key);
            auto const flowPath = (fs::path(domain.path) / (flow.flow_id + ".mxl-flow")).string();
            if (flowLogged_.count(key) == 0)
            {
                log::info("mirror_flow_origin_missing", {{"domain_id", domain.domain_id}, {"flow_id", flow.flow_id},
                    {"peer", domain.source_host_id}, {"grace_s", std::to_string(cfg.mirror_idle_grace_s)}});
                flowLogged_[key] = "missing";
            }
            // The scanner's snapshot may still list a flow that is gone meanwhile.
            if (plan.remove && fs::is_directory(flowPath))
            {
                candidates.push_back(Candidate{key, domain.domain_id, flow.flow_id, domain.path, flowPath, plan.reason,
                    std::chrono::duration_cast<std::chrono::seconds>(now - since).count()});
            }
        }
    }
    // A flow offered again (or whose source went down) starts a new grace time when its origin goes missing.
    for (auto it = flowMissingSince_.begin(); it != flowMissingSince_.end();)
    {
        it = missing.count(it->first) == 0 ? flowMissingSince_.erase(it) : std::next(it);
    }
    for (auto it = flowLogged_.begin(); it != flowLogged_.end();)
    {
        it = missing.count(it->first) == 0 ? flowLogged_.erase(it) : std::next(it);
    }
    // Removal candidates: check the readers (at most every 2 s), then remove what no other process maps.
    if (candidates.empty() || now < nextFlowReaderScan_)
    {
        return;
    }
    nextFlowReaderScan_ = now + std::chrono::seconds(2);
    if (cfg.mirror_cleanup_dry_run)
    {
        for (auto const& c : candidates)
        {
            if (flowLogged_[c.key] != "dry_run")
            {
                log::info("mirror_flow_would_remove", {{"domain_id", c.domain_id}, {"flow_id", c.flow_id}, {"path", c.flow_path},
                    {"reason", c.reason}, {"missing_s", std::to_string(c.missing_s)}});
                flowLogged_[c.key] = "dry_run";
            }
        }
        return;
    }
    // Per flow: the files of one flow directory, matched by device and inode in /proc/<pid>/maps of every
    // other process (one pass for all candidates). A flow some process maps stays.
    auto const scanFlows = [](std::vector<Candidate const*> const& flows) {
        std::map<FileKey, std::string> files;
        for (auto const* c : flows)
        {
            collectFiles(c->flow_path, c->flow_path, files);
        }
        return scanReaders(files, "/proc", static_cast<int>(::getpid()));
    };
    auto const keep = [&](Candidate const& c, std::string const& refused) {
        if (flowLogged_[c.key] != "refused:" + refused)
        {
            log::warn("mirror_flow_kept_reader", {{"domain_id", c.domain_id}, {"flow_id", c.flow_id}, {"path", c.flow_path},
                {"reason", c.reason}, {"refused", refused}});
            metrics_.addCounter("mirror_flows_refused_total", 1, {{"reason", c.reason}});
            flowLogged_[c.key] = "refused:" + refused;
        }
    };
    std::vector<Candidate const*> all;
    for (auto const& c : candidates)
    {
        all.push_back(&c);
    }
    std::vector<Candidate const*> free;
    auto scan = scanFlows(all);
    for (auto const* c : all)
    {
        std::vector<ReaderProcess> readers;
        if (auto const refused = removalBlocker(scan, c->flow_path, &readers); !refused.empty())
        {
            keep(*c, refused);
        }
        else
        {
            free.push_back(c);
        }
    }
    if (free.empty())
    {
        return;
    }
    // The agent's own writer first (a target went with the pull), then a last reader check right before the
    // files go.
    for (auto const* c : free)
    {
        engine_.releaseWriter(c->domain_path, c->flow_id);
    }
    scan = scanFlows(free);
    for (auto const* c : free)
    {
        std::vector<ReaderProcess> readers;
        if (auto const refused = removalBlocker(scan, c->flow_path, &readers); !refused.empty())
        {
            keep(*c, refused);
            continue;
        }
        std::error_code ec;
        fs::remove_all(c->flow_path, ec);
        if (ec)
        {
            log::warn("mirror_flow_remove_failed", {{"domain_id", c->domain_id}, {"flow_id", c->flow_id}, {"path", c->flow_path},
                {"error", ec.message()}});
            continue;
        }
        mirrorHealth_.noteRemoved(c->key, c->reason);
        metrics_.addCounter("mirror_flows_removed_total", 1, {{"reason", c->reason}});
        log::info("mirror_flow_removed", {{"domain_id", c->domain_id}, {"flow_id", c->flow_id}, {"path", c->flow_path},
            {"reason", c->reason}, {"missing_s", std::to_string(c->missing_s)}});
        flowLogged_[c->key] = "removed"; // forgotten once the scanner no longer lists it
    }
}

void Controller::observeMirrors(Config const& cfg, Inventory const& inventory, std::vector<PeerSnapshot> const& peerSnap,
    std::vector<MirrorObservation> observations, std::set<std::string> const& seen, bool inventoriesSettled)
{
    auto const now = std::chrono::steady_clock::now();
    auto const tai = taiNowSeconds();
    auto const unixMs = unixNowMs();
    // Mirror flows on disk that no up peer offers: a source peer that is down keeps the expectation of
    // grains while the demand stays (the mirror is stale); a source that is up no longer has the origin.
    for (auto const& domain : inventory.domains)
    {
        if (domain.kind != "mirror" || fs::path(domain.path).filename().string() != "mirror-" + domain.domain_id)
        {
            continue;
        }
        bool sourceUp = false;
        for (auto const& peer : peerSnap)
        {
            if (peer.host_id == domain.source_host_id)
            {
                sourceUp = peer.up;
            }
        }
        for (auto const& flow : domain.flows)
        {
            auto const key = domain.domain_id + "/" + flow.flow_id;
            if (seen.count(key) != 0)
            {
                continue;
            }
            MirrorObservation obs;
            obs.domain_id = domain.domain_id;
            obs.flow_id = flow.flow_id;
            obs.peer = domain.source_host_id;
            obs.rate_num = flow.grain_rate_num;
            obs.rate_den = flow.grain_rate_den;
            obs.idle_reason = unofferedMirrorReason(sourceUp, inventoriesSettled);
            if (!sourceUp)
            {
                obs.blocked = "peer_down";
                obs.expected = mirrorHealth_.expected(key) && lastDemand_.count(key) != 0;
            }
            observations.push_back(std::move(obs));
        }
    }
    std::set<std::string> observed;
    std::vector<MirrorEvent> events;
    for (auto& obs : observations)
    {
        auto const key = obs.domain_id + "/" + obs.flow_id;
        if (!observed.insert(key).second)
        {
            continue;
        }
        readMirrorFile(mirrorPath(cfg.mxl_root, obs.domain_id), obs.flow_id, obs);
        if (obs.inode == 0 && mirrorHealth_.entries().count(key) == 0)
        {
            continue; // a peer flow without a mirror here
        }
        obs.tai_now = tai;
        auto more = mirrorHealth_.update(obs, cfg.mirror_stale_ms, now, unixMs);
        events.insert(events.end(), more.begin(), more.end());
    }
    // Mirrors seen before and neither offered nor on disk now: absent once, forgotten after a day.
    std::vector<MirrorObservation> gone;
    for (auto const& [key, health] : mirrorHealth_.entries())
    {
        if (observed.count(key) == 0 && health.state != "absent")
        {
            MirrorObservation obs;
            obs.domain_id = health.domain_id;
            obs.flow_id = health.flow_id;
            gone.push_back(std::move(obs));
        }
    }
    for (auto const& obs : gone)
    {
        auto more = mirrorHealth_.update(obs, cfg.mirror_stale_ms, now, unixMs);
        events.insert(events.end(), more.begin(), more.end());
    }
    mirrorHealth_.prune(now, std::chrono::hours(24));
    reportMirrorEvents(events, cfg.mirror_stale_ms);
}

void Controller::reportMirrorEvents(std::vector<MirrorEvent> const& events, int staleMs)
{
    for (auto const& event : events)
    {
        auto const& h = event.health;
        std::vector<log::Field> fields{{"domain_id", h.domain_id}, {"flow_id", h.flow_id}, {"peer", h.peer}};
        auto const age = h.last_grain_age_ms ? std::to_string(static_cast<std::int64_t>(std::llround(*h.last_grain_age_ms))) : std::string();
        if (event.kind == "stale")
        {
            fields.push_back({"reason", h.reason});
            fields.push_back({"last_grain_age_ms", age});
            fields.push_back({"stale_after_ms", std::to_string(staleMs)});
            log::warn("mirror_stale", fields);
        }
        else if (event.kind == "recovered")
        {
            fields.push_back({"state", h.state});
            fields.push_back({"reason", h.reason});
            fields.push_back({"stale_ms", std::to_string(static_cast<std::int64_t>(std::llround(event.stale_ms)))});
            log::info("mirror_recovered", fields);
        }
        else
        {
            fields.push_back({"reason", h.last_rebuild_reason});
            fields.push_back({"rebuilds", std::to_string(h.rebuilds)});
            fields.push_back({"old_inode", std::to_string(event.old_inode)});
            fields.push_back({"inode", std::to_string(h.inode)});
            log::warn("mirror_rebuilt", fields);
        }
        picojson::object data;
        data["event"] = picojson::value(event.kind);
        data["domain_id"] = picojson::value(h.domain_id);
        data["flow_id"] = picojson::value(h.flow_id);
        data["peer"] = picojson::value(h.peer);
        putHealth(data, h, staleMs);
        if (event.kind == "recovered")
        {
            data["stale_ms"] = picojson::value(std::round(event.stale_ms));
        }
        publish("mirror", picojson::value(data).serialize());
    }
}

void Controller::renderMetrics()
{
    auto const cfg = store_->get();
    auto const inventory = scanner_.snapshot();
    auto const peerSnap = peers_.snapshot();
    auto const nmos = observer_.snapshot();
    int local = 0;
    int mirror = 0;
    int conflict = 0;
    int originActive = 0;
    int originIdle = 0;
    for (auto const& domain : inventory.domains)
    {
        if (domain.kind == "local")
        {
            ++local;
        }
        else if (domain.kind == "mirror")
        {
            ++mirror;
        }
        else if (domain.kind == "conflict")
        {
            ++conflict;
        }
        if (domain.kind == "local")
        {
            for (auto const& flow : domain.flows)
            {
                if (flow.active)
                {
                    ++originActive;
                }
                else
                {
                    ++originIdle;
                }
            }
        }
    }
    metrics_.setGauge("info", 1, {{"host_id", cfg.host_id}, {"version", kVersion}, {"mxl_version", kMxlPinName}, {"libfabric_version", "2.3"}});
    metrics_.setGauge("domains", local, {{"kind", "local"}});
    metrics_.setGauge("domains", mirror, {{"kind", "mirror"}});
    metrics_.setGauge("domains", conflict, {{"kind", "conflict"}});
    {
        // Destination mirror domains on disk by cleanup state.
        std::map<std::string, int> byState{{"active", 0}, {"idle", 0}, {"orphan", 0}, {"same_node", 0}};
        {
            std::lock_guard const lock{mu_};
            for (auto const& view : mirrorDomains_)
            {
                if (view.on_disk)
                {
                    ++byState[view.state];
                }
            }
        }
        std::vector<std::pair<std::map<std::string, std::string>, double>> series;
        for (auto const& [state, count] : byState)
        {
            series.push_back({{{"state", state}}, static_cast<double>(count)});
        }
        metrics_.setGauges("mirror_domains", series);
    }
    metrics_.setGauge("flows", originActive, {{"kind", "origin"}, {"state", "active"}});
    metrics_.setGauge("flows", originIdle, {{"kind", "origin"}, {"state", "inactive"}});
    struct statvfs st{};
    if (statvfs(cfg.mxl_root.c_str(), &st) == 0)
    {
        metrics_.setGauge("tmpfs_free_bytes", static_cast<double>(st.f_bavail) * static_cast<double>(st.f_frsize));
        metrics_.setGauge("tmpfs_size_bytes", static_cast<double>(st.f_blocks) * static_cast<double>(st.f_frsize));
    }
    std::map<std::string, FabricLink> links;
    {
        std::lock_guard const lock{mu_};
        links = links_;
    }
    // A peer is up when its control plane answers and the local fabric link to it has carrier.
    int up = 0;
    std::vector<std::pair<std::map<std::string, std::string>, double>> peerUp;
    for (auto const& peer : peerSnap)
    {
        auto const& link = links[peer.host_id];
        bool const usable = peer.up && link.up;
        if (usable)
        {
            ++up;
        }
        peerUp.push_back({{{"peer", peer.host_id}, {"interface", link.netdev}}, usable ? 1.0 : 0.0});
        if (link.retransmits)
        {
            metrics_.setCounter("peer_rdma_retransmits_total", static_cast<double>(*link.retransmits), {{"peer", peer.host_id}, {"device", link.rdmaDevice}});
        }
    }
    metrics_.setGauges("peer_up", peerUp);
    metrics_.setGauge("peers_up", up);
    metrics_.setGauge("origin_conflicts", static_cast<double>(originConflicts_.size()));
    metrics_.setGauge("nmos_registry_up", nmos.registry_up ? 1 : 0);
    metrics_.setGauge("tai_offset_seconds", tai_);
    std::map<std::string, int> demandStates;
    for (auto const& entry : demand_.entries)
    {
        demandStates[entry.state] += 1;
    }
    for (auto const& [state, count] : demandStates)
    {
        metrics_.setGauge("demand_entries", count, {{"state", state}});
    }
    std::map<std::string, int> reps;
    std::vector<std::pair<std::map<std::string, std::string>, double>> states;
    for (auto const& row : engine_.status())
    {
        if (row.state == "active" || row.state == "pending")
        {
            reps[row.role + "/" + row.provider] += 1;
        }
        if (cfg.metrics_per_flow)
        {
            states.push_back({{{"flow_id", row.flow_id}, {"peer", row.peer}, {"role", row.role}, {"state", row.state}}, 1});
            metrics_.setGauge("replication_lag_grains", static_cast<double>(row.lag), {{"flow_id", row.flow_id}, {"peer", row.peer}});
            metrics_.setGauge("replication_lag_ms", row.lag_ms, {{"flow_id", row.flow_id}, {"peer", row.peer}});
            metrics_.setGauge("completion_queue_depth", row.cq_depth, {{"flow_id", row.flow_id}});
            // The totals live in the replication rows (they start again with a new replication).
            std::map<std::string, std::string> const labels{{"flow_id", row.flow_id}, {"peer", row.peer}, {"role", row.role}};
            metrics_.setCounter("replication_grains_total", static_cast<double>(row.grains), labels);
            metrics_.setCounter("replication_bytes_total", static_cast<double>(row.bytes), labels);
            metrics_.setCounter("replication_errors_total", static_cast<double>(row.errors), labels);
            if (row.role == "destination")
            {
                metrics_.setCounter("replication_restarts_total", static_cast<double>(row.restarts), {{"flow_id", row.flow_id}, {"peer", row.peer}});
            }
            else
            {
                metrics_.setCounter("replication_origin_gaps_total", static_cast<double>(row.gaps), {{"flow_id", row.flow_id}, {"peer", row.peer}});
            }
        }
    }
    for (auto const& [key, count] : reps)
    {
        auto const slash = key.find('/');
        metrics_.setGauge("replications_active", count, {{"role", key.substr(0, slash)}, {"provider", key.substr(slash + 1)}});
    }
    // One series per replication with its current state (1); a former state disappears.
    metrics_.setGauges("replication_state", states);
    // Destination mirror flows: stale, state, age of the newest grain, rebuilds.
    std::map<std::string, MirrorHealth> health;
    {
        std::lock_guard const lock{mu_};
        health = mirrorHealthView_;
    }
    int staleCount = 0;
    std::uint64_t rebuildsSum = 0;
    std::vector<std::pair<std::map<std::string, std::string>, double>> staleSeries;
    std::vector<std::pair<std::map<std::string, std::string>, double>> stateSeries;
    std::vector<std::pair<std::map<std::string, std::string>, double>> ageSeries;
    for (auto const& [key, h] : health)
    {
        std::map<std::string, std::string> const labels{{"domain_id", h.domain_id}, {"flow_id", h.flow_id}, {"peer", h.peer}};
        rebuildsSum += h.rebuilds;
        if (cfg.metrics_per_flow)
        {
            metrics_.setCounter("mirror_rebuilds_total", static_cast<double>(h.rebuilds), labels);
        }
        if (h.state == "absent")
        {
            continue;
        }
        staleCount += h.state == "stale" ? 1 : 0;
        if (cfg.metrics_per_flow)
        {
            staleSeries.push_back({labels, h.state == "stale" ? 1.0 : 0.0});
            auto withState = labels;
            withState["state"] = h.state;
            stateSeries.push_back({withState, 1.0});
            if (h.last_grain_age_ms)
            {
                ageSeries.push_back({labels, *h.last_grain_age_ms / 1000.0});
            }
        }
    }
    if (!cfg.metrics_per_flow)
    {
        metrics_.setCounter("mirror_rebuilds_total", static_cast<double>(rebuildsSum));
    }
    metrics_.setGauges("mirror_stale", staleSeries);
    metrics_.setGauges("mirror_state", stateSeries);
    metrics_.setGauges("mirror_last_grain_age_seconds", ageSeries);
    metrics_.setGauge("mirrors_stale", staleCount);
}

void Controller::stream(SseEmit const& emit)
{
    std::size_t cursor = 0;
    while (true)
    {
        std::vector<Event> batch;
        {
            std::unique_lock lock{eventMu_};
            eventCv_.wait_for(lock, std::chrono::seconds(15), [&] { return eventsStop_ || events_.size() > cursor; });
            if (eventsStop_)
            {
                return;
            }
            if (events_.size() <= cursor)
            {
                lock.unlock();
                if (!emit("ping", "{}"))
                {
                    return;
                }
                continue;
            }
            batch.assign(events_.begin() + static_cast<std::ptrdiff_t>(cursor), events_.end());
            cursor = events_.size();
        }
        for (auto const& event : batch)
        {
            if (!emit(event.name, event.data))
            {
                return;
            }
        }
    }
}

HttpResponse Controller::handle(HttpRequest const& request)
{
    auto const cfg = store_->get();
    auto json = [](int status, std::string body) {
        return HttpResponse{status, "application/json", std::move(body), false};
    };
    if (request.method == "GET" && request.path == "/livez")
    {
        return json(200, "{\"status\":\"live\"}");
    }
    if (request.method == "GET" && request.path == "/readyz")
    {
        bool const registryRequired = cfg.nmos_enable && cfg.registryConfigured();
        bool const registered = nmosRegistered_.load() || observer_.snapshot().registered;
        bool const nmosOk = !cfg.nmos_enable || (nmosReady_.load() && (!registryRequired || registered));
        bool const ok = ready_.load() && scanner_.running() && nmosOk;
        return json(ok ? 200 : 503, ok ? "{\"status\":\"ready\"}" : "{\"status\":\"not ready\"}");
    }
    if (request.method == "GET" && request.path == "/metrics")
    {
        return HttpResponse{200, "text/plain; version=0.0.4", metrics_.render(), false};
    }
    if (request.method == "GET" && request.path == "/statusz")
    {
        picojson::object root;
        root["host_id"] = picojson::value(cfg.host_id);
        root["boot_id"] = picojson::value(bootId_);
        root["ready"] = picojson::value(ready_.load());
        root["nmos"] = picojson::value(nmosReady_.load());
        root["nmos_registered"] = picojson::value(nmosRegistered_.load() || observer_.snapshot().registered);
        root["nmos_error"] = picojson::value(nmosError_);
        root["tai_offset_seconds"] = picojson::value(static_cast<double>(tai_));
        root["mxl_root_tmpfs"] = picojson::value(rootTmpfs_);
        return json(200, picojson::value(root).serialize());
    }
    if (request.method == "GET" && (request.path == "/" || request.path == "/index.html"))
    {
        if (!cfg.web_enable)
        {
            return json(404, "{\"error\":\"ui disabled\"}");
        }
#ifdef MFA_HAS_UI
        return HttpResponse{200, "text/html; charset=utf-8", std::string(reinterpret_cast<char const*>(kIndexHtml), kIndexHtmlSize), false};
#else
        return HttpResponse{200, "text/html; charset=utf-8", kFallbackUi, false};
#endif
    }
    if (request.path.rfind("/api/v1", 0) != 0)
    {
        return json(404, "{\"error\":\"not found\"}");
    }
    if (request.method == "GET" && request.path == "/api/v1/info")
    {
        picojson::array providers;
        picojson::object body;
        body["host_id"] = picojson::value(cfg.host_id);
        body["boot_id"] = picojson::value(bootId_);
        body["version"] = picojson::value(kVersion);
        body["mxl_version"] = picojson::value(std::string(kMxlPinName) + " " + kMxlPin);
        body["libfabric_version"] = picojson::value(std::string("2.3.1"));
        body["providers"] = picojson::value(providers);
        body["tai_offset_seconds"] = picojson::value(static_cast<double>(tai_));
        body["mxl_root_tmpfs"] = picojson::value(rootTmpfs_);
        return json(200, picojson::value(body).serialize());
    }
    if (request.method == "GET" && request.path == "/api/v1/inventory")
    {
        // Each local flow's head index now (not part of the revision): a destination tells a mirror that
        // misses grains from an origin that writes none.
        auto inventory = scanner_.snapshot();
        // Before the first scan the inventory is empty, not the truth: a peer would miss every flow (1.7.1).
        if (inventory.revision == 0)
        {
            return json(503, R"({"error":"inventory not scanned yet"})");
        }
        for (auto& domain : inventory.domains)
        {
            if (domain.kind != "local")
            {
                continue;
            }
            for (auto& flow : domain.flows)
            {
                auto const header = readFlowHeader((fs::path(domain.path) / (flow.flow_id + ".mxl-flow") / "data").string());
                if (header.headIndex != 0 && header.headIndex != std::numeric_limits<std::uint64_t>::max())
                {
                    flow.head_index = header.headIndex;
                }
            }
        }
        return json(200, inventory.canonicalJson());
    }
    if (request.method == "GET" && request.path == "/api/v1/mirrors")
    {
        std::vector<MirrorDomainView> views;
        std::map<std::string, MirrorHealth> health;
        {
            std::lock_guard const lock{mu_};
            views = mirrorDomains_;
            health = mirrorHealthView_;
        }
        // Readers now: every process other than the agent that maps a file of a mirror domain.
        std::map<FileKey, std::string> files;
        for (auto const& view : views)
        {
            if (view.on_disk)
            {
                collectFiles(view.path, view.path, files);
            }
        }
        auto const scan = scanReaders(files, "/proc", static_cast<int>(::getpid()));
        auto const nowUnix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        picojson::array arr;
        for (auto const& view : views)
        {
            picojson::object obj;
            obj["domain_id"] = picojson::value(view.domain_id);
            obj["path"] = picojson::value(view.path);
            obj["source_host_id"] = picojson::value(view.source_host_id);
            picojson::object origin;
            origin["host_id"] = picojson::value(view.source_host_id);
            origin["peer_up"] = picojson::value(view.source_up);
            origin["on_peer"] = picojson::value(view.origin_on_peer);
            origin["local"] = picojson::value(view.origin_local);
            obj["origin"] = picojson::value(origin);
            obj["state"] = picojson::value(view.state);
            obj["on_disk"] = picojson::value(view.on_disk);
            obj["demand"] = picojson::value(view.demand);
            obj["idle_since"] = view.idle_since_unix > 0 ? picojson::value(static_cast<double>(view.idle_since_unix)) : picojson::value();
            obj["age_s"] = view.created_unix > 0 ? picojson::value(static_cast<double>(nowUnix - view.created_unix)) : picojson::value();
            picojson::array readers;
            if (auto const it = scan.readers.find(view.path); it != scan.readers.end())
            {
                for (auto const& reader : it->second)
                {
                    picojson::object r;
                    r["pid"] = picojson::value(static_cast<double>(reader.pid));
                    r["comm"] = picojson::value(reader.comm);
                    readers.push_back(picojson::value(r));
                }
            }
            obj["readers"] = picojson::value(readers);
            obj["readers_known"] = picojson::value(scan.complete);
            if (!scan.complete)
            {
                obj["readers_error"] = picojson::value(scan.error);
            }
            picojson::array flows;
            for (auto const& [flow, state] : view.flows)
            {
                picojson::object f;
                f["flow_id"] = picojson::value(flow.flow_id);
                f["media_type"] = picojson::value(flow.media_type);
                f["format"] = picojson::value(flow.format);
                f["active"] = picojson::value(flow.active);
                f["state"] = picojson::value(state);
                // Stale and rebuilt (for the platform's re-routing), as of the last reconcile pass.
                picojson::object h;
                if (auto const it = health.find(view.domain_id + "/" + flow.flow_id); it != health.end())
                {
                    putHealth(h, it->second, cfg.mirror_stale_ms);
                }
                else
                {
                    putHealth(h, MirrorHealth{}, cfg.mirror_stale_ms);
                }
                f["health"] = picojson::value(h);
                flows.push_back(picojson::value(f));
            }
            obj["flows"] = picojson::value(flows);
            arr.push_back(picojson::value(obj));
        }
        return json(200, picojson::value(arr).serialize());
    }
    if (request.method == "POST")
    {
        if (auto const rebuild = parseMirrorRebuildPath(request.path); rebuild.matched)
        {
            return rebuildMirror(rebuild);
        }
    }
    if (request.method == "DELETE" && request.path.rfind("/api/v1/mirrors/", 0) == 0)
    {
        auto const id = request.path.substr(std::string("/api/v1/mirrors/").size());
        std::lock_guard const tickLock{tickMu_};
        auto const cfg = store_->get();
        DomainRecord const* found = nullptr;
        auto const inventory = scanner_.snapshot();
        for (auto const& domain : inventory.domains)
        {
            if (domain.domain_id == id && domain.kind == "mirror" && fs::path(domain.path).filename().string() == "mirror-" + id)
            {
                found = &domain;
            }
        }
        picojson::object body;
        body["domain_id"] = picojson::value(id);
        if (found == nullptr || !fs::is_directory(found->path))
        {
            body["error"] = picojson::value("no mirror domain of this agent with this id");
            return json(404, picojson::value(body).serialize());
        }
        body["path"] = picojson::value(found->path);
        if (cfg.mirror_cleanup_dry_run)
        {
            log::info("mirror_domain_would_remove", {{"domain_id", id}, {"path", found->path}, {"reason", "api"}});
            body["removed"] = picojson::value(false);
            body["dry_run"] = picojson::value(true);
            return json(200, picojson::value(body).serialize());
        }
        std::vector<ReaderProcess> readers;
        auto const refused = removeMirrorDomain(id, found->path, "api", &readers);
        if (!refused.empty())
        {
            metrics_.addCounter("mirror_domains_refused_total", 1, {{"reason", "api"}});
            log::warn("mirror_domain_busy", {{"domain_id", id}, {"path", found->path}, {"reason", "api"}, {"refused", refused}});
            body["removed"] = picojson::value(false);
            body["error"] = picojson::value(refused);
            picojson::array pids;
            for (auto const& reader : readers)
            {
                picojson::object r;
                r["pid"] = picojson::value(static_cast<double>(reader.pid));
                r["comm"] = picojson::value(reader.comm);
                pids.push_back(picojson::value(r));
            }
            body["readers"] = picojson::value(pids);
            return json(409, picojson::value(body).serialize());
        }
        metrics_.addCounter("mirror_domains_removed_total", 1, {{"reason", "api"}});
        body["removed"] = picojson::value(true);
        return json(200, picojson::value(body).serialize());
    }
    if (request.method == "GET" && request.path == "/api/v1/demand")
    {
        std::lock_guard const lock{mu_};
        picojson::object root;
        picojson::array receivers;
        for (auto const& receiver : demand_.receivers)
        {
            picojson::object obj;
            obj["id"] = picojson::value(receiver.id);
            obj["label"] = picojson::value(receiver.label);
            obj["node_id"] = picojson::value(receiver.node_id);
            obj["node_label"] = picojson::value(receiver.node_label);
            obj["master_enable"] = picojson::value(receiver.active.master_enable);
            obj["mxl_domain_id"] = picojson::value(receiver.active.domain_id);
            obj["mxl_flow_id"] = picojson::value(receiver.active.flow_id);
            std::string state = "local";
            for (auto const& entry : demand_.entries)
            {
                if (std::find(entry.receiver_ids.begin(), entry.receiver_ids.end(), receiver.id) != entry.receiver_ids.end())
                {
                    state = entry.state;
                    obj["source_host"] = picojson::value(entry.source_host);
                }
            }
            obj["state"] = picojson::value(state);
            receivers.push_back(picojson::value(obj));
        }
        root["receivers"] = picojson::value(receivers);
        return json(200, picojson::value(root).serialize());
    }
    if (request.method == "GET" && request.path == "/api/v1/replications")
    {
        picojson::array arr;
        for (auto const& row : engine_.status())
        {
            picojson::object obj;
            obj["replication_id"] = picojson::value(row.replication_id);
            obj["role"] = picojson::value(row.role);
            obj["domain_id"] = picojson::value(row.domain_id);
            obj["flow_id"] = picojson::value(row.flow_id);
            obj["peer"] = picojson::value(row.peer);
            obj["provider"] = picojson::value(row.provider);
            obj["state"] = picojson::value(row.state);
            obj["grains"] = picojson::value(static_cast<double>(row.grains));
            obj["bytes"] = picojson::value(static_cast<double>(row.bytes));
            obj["errors"] = picojson::value(static_cast<double>(row.errors));
            obj["restarts"] = picojson::value(static_cast<double>(row.restarts));
            obj["head_index"] = picojson::value(static_cast<double>(row.head));
            obj["lag_grains"] = picojson::value(static_cast<double>(row.lag));
            obj["lag_ms"] = picojson::value(row.lag_ms);
            obj["origin_gaps"] = picojson::value(static_cast<double>(row.gaps));
            obj["last_error"] = picojson::value(row.last_error);
            obj["fallback"] = picojson::value(row.fallback);
            arr.push_back(picojson::value(obj));
        }
        return json(200, picojson::value(arr).serialize());
    }
    if (request.method == "POST" && request.path == "/api/v1/replications")
    {
        auto const body = json::parse(request.body);
        if (!body.is<picojson::object>())
        {
            return json(400, "{\"error\":\"expected object\"}");
        }
        auto const& obj = body.get<picojson::object>();
        PostRequest post;
        post.domain_id = json::asString(obj, "domain_id").value_or("");
        post.flow_id = json::asString(obj, "flow_id").value_or("");
        post.dest_host_id = json::asString(obj, "dest_host_id").value_or("");
        post.target_info = json::asString(obj, "target_info").value_or("");
        if (post.domain_id.empty() || post.flow_id.empty() || post.dest_host_id.empty() || post.target_info.empty())
        {
            return json(400, "{\"error\":\"domain_id, flow_id, dest_host_id and target_info are required\"}");
        }
        std::string domainPath;
        for (auto const& domain : scanner_.snapshot().domains)
        {
            if (domain.kind == "local" && domain.domain_id == post.domain_id)
            {
                domainPath = domain.path;
            }
        }
        if (domainPath.empty())
        {
            return json(404, "{\"error\":\"domain not local\"}");
        }
        auto const ep = cfg.peers;
        for (auto const& peer : ep)
        {
            if (peer.host_id == post.dest_host_id)
            {
                post.local_addr = peer.local_fabric_addr;
                post.provider = peer.provider;
            }
        }
        auto const result = engine_.post(post, domainPath);
        picojson::object response;
        response["replication_id"] = picojson::value(result.replication_id);
        response["state"] = picojson::value(result.state);
        if (!result.error.empty())
        {
            response["error"] = picojson::value(result.error);
        }
        if (result.origin_head)
        {
            response["origin_head"] = picojson::value(static_cast<double>(*result.origin_head));
        }
        publish("replication", picojson::value(response).serialize());
        return json(result.status == 0 ? 201 : result.status, picojson::value(response).serialize());
    }
    if (request.method == "DELETE" && request.path.rfind("/api/v1/replications/", 0) == 0)
    {
        auto rest = request.path.substr(std::string("/api/v1/replications/").size());
        auto const slash = rest.find("/targets/");
        if (slash == std::string::npos)
        {
            return json(400, "{\"error\":\"expected /replications/{id}/targets/{dest}\"}");
        }
        auto const id = rest.substr(0, slash);
        auto const dest = rest.substr(slash + std::string("/targets/").size());
        if (!engine_.eraseTarget(id, dest))
        {
            return json(404, "{\"error\":\"not found\"}");
        }
        return HttpResponse{204, "application/json", "", false};
    }
    if (request.method == "GET" && request.path == "/api/v1/peers")
    {
        std::map<std::string, FabricLink> links;
        {
            std::lock_guard const lock{mu_};
            links = links_;
        }
        picojson::array arr;
        for (auto const& peer : peers_.snapshot())
        {
            auto const& link = links[peer.host_id];
            picojson::object obj;
            obj["host_id"] = picojson::value(peer.host_id);
            obj["control_url"] = picojson::value(peer.control_url);
            obj["up"] = picojson::value(peer.up);
            obj["link_up"] = picojson::value(link.up);
            obj["link_error"] = picojson::value(link.error);
            obj["fabric_interface"] = picojson::value(link.netdev);
            obj["boot_id"] = picojson::value(peer.boot_id);
            obj["provider"] = picojson::value(peer.provider);
            obj["local_fabric_addr"] = picojson::value(peer.local_addr);
            obj["remote_fabric_addr"] = picojson::value(peer.remote_addr);
            obj["error"] = picojson::value(peer.error);
            arr.push_back(picojson::value(obj));
        }
        return json(200, picojson::value(arr).serialize());
    }
    if (request.method == "POST" && request.path.rfind("/api/v1/peers/", 0) == 0 && request.path.size() > 16 &&
        request.path.find("/test") != std::string::npos)
    {
        auto id = request.path.substr(std::string("/api/v1/peers/").size());
        auto const cut = id.find("/test");
        id = id.substr(0, cut);
        return testPeer(id);
    }
    if (request.method == "GET" && request.path == "/api/v1/config")
    {
        picojson::object root;
        std::string err;
        root["config"] = json::parse(store_->exportJson(), &err);
        picojson::object origin;
        for (auto const& [key, where] : store_->origins())
        {
            origin[key] = picojson::value(where == ValueOrigin::Env ? "env" : where == ValueOrigin::File ? "file" : "default");
        }
        root["origin"] = picojson::value(origin);
        root["restart_required"] = picojson::value(store_->restartRequired());
        return json(200, picojson::value(root).serialize());
    }
    if (request.method == "GET" && request.path == "/api/v1/config/env")
    {
        return HttpResponse{200, "text/plain", store_->exportEnv(), false};
    }
    if (request.method == "GET" && request.path == "/api/v1/config/export")
    {
        return json(200, store_->exportJson());
    }
    if (request.method == "POST" && request.path == "/api/v1/config/import")
    {
        try
        {
            auto body = request.body;
            std::string err;
            auto const parsed = json::parse(body, &err);
            if (!err.empty() || !parsed.is<picojson::object>())
            {
                return json(400, "{\"error\":\"config body is not a JSON object\"}");
            }
            auto const& obj = parsed.get<picojson::object>();
            if (obj.size() == 1 && obj.count("config") != 0 && obj.at("config").is<picojson::object>())
            {
                body = obj.at("config").serialize();
            }
            store_->importDocument(loadConfigFileText(body));
            engine_.updateConfig(store_->get());
            peers_.updateConfig(store_->get());
            observer_.updateConfig(store_->get());
            return json(200, std::string("{\"restart_required\":") + (store_->restartRequired() ? "true" : "false") + "}");
        }
        catch (ConfigError const& ex)
        {
            return json(400, std::string("{\"error\":\"") + ex.what() + "\"}");
        }
    }
    if (request.method == "PUT" && request.path == "/api/v1/config")
    {
        if (!cfg.web_enable)
        {
            return json(403, "{\"error\":\"ui disabled\"}");
        }
        try
        {
            auto const patch = loadConfigFileText(request.body);
            bool restart = false;
            store_->updateFile(patch, &restart);
            engine_.updateConfig(store_->get());
            peers_.updateConfig(store_->get());
            return json(200, std::string("{\"restart_required\":") + (restart ? "true" : "false") + "}");
        }
        catch (ConfigError const& ex)
        {
            return json(400, std::string("{\"error\":\"") + ex.what() + "\"}");
        }
    }
    return json(404, "{\"error\":\"not found\"}");
}

HttpResponse Controller::testPeer(std::string const& hostId)
{
    PeerSnapshot peer;
    bool found = false;
    for (auto const& item : peers_.snapshot())
    {
        if (item.host_id == hostId)
        {
            peer = item;
            found = true;
        }
    }
    if (!found)
    {
        for (auto const& item : store_->get().peers)
        {
            if (item.host_id == hostId)
            {
                peer.host_id = item.host_id;
                peer.control_url = item.control_url;
                peer.local_addr = item.local_fabric_addr;
                peer.remote_addr = item.remote_fabric_addr;
                peer.provider = item.provider;
                found = true;
            }
        }
    }
    picojson::object body;
    body["host_id"] = picojson::value(hostId);
    if (!found || peer.control_url.empty())
    {
        body["ok"] = picojson::value(false);
        body["error"] = picojson::value("peer control url is unknown");
        return HttpResponse{200, "application/json", picojson::value(body).serialize(), false};
    }
    auto const info = httpRequest("GET", peer.control_url + "/info");
    body["control_up"] = picojson::value(info.status == 200);
    if (info.status != 200)
    {
        body["ok"] = picojson::value(false);
        body["error"] = picojson::value(info.error.empty() ? "control plane down" : info.error);
        return HttpResponse{200, "application/json", picojson::value(body).serialize(), false};
    }
    auto const cfg = store_->get();
    auto const id = uuidV4().str();
    auto const flow = testFlow(id);
    auto const localDir = fs::temp_directory_path() / ("mfa-test-" + cfg.host_id);
    auto const options = std::string("{\"urn:x-mxl:option:history_duration/v1.0\":1000000000}");
    writeText(localDir / "options.json", options);
    writeText(localDir / "domain_def.json", std::string("{\"id\":\"") + id + "\"}");
    std::string error;
    if (!engine_.ensureWriter(localDir.string(), flow, &error))
    {
        body["ok"] = picojson::value(false);
        body["error"] = picojson::value(error);
        return HttpResponse{200, "application/json", picojson::value(body).serialize(), false};
    }
    auto dom = engine_.domain(localDir.string());
    std::uint64_t index = 0;
    if (!dom->commitPattern(id, &index, &error))
    {
        body["ok"] = picojson::value(false);
        body["error"] = picojson::value(error);
        return HttpResponse{200, "application/json", picojson::value(body).serialize(), false};
    }
    FabricEndpoint ep;
    ep.provider = peer.provider.empty() ? cfg.default_provider : peer.provider;
    ep.node = peer.local_addr.empty() ? cfg.fabric_interface : peer.local_addr;
    ep.service = std::to_string(cfg.fabric_port_base + 1);
    ep.allowTcpFallback = cfg.provider_fallback == "tcp";
    auto const setup = dom->setupTarget("self-test", id, ep, 32);
    body["provider"] = picojson::value(setup.provider_used);
    body["fallback"] = picojson::value(setup.fallback);
    body["ok"] = picojson::value(setup.ok);
    if (!setup.ok)
    {
        body["error"] = picojson::value(setup.error);
    }
    else
    {
        body["detail"] = picojson::value("local target setup succeeded; grain " + std::to_string(index));
    }
    dom->destroyTarget("self-test");
    engine_.releaseWriter(localDir.string(), id);
    std::error_code ec;
    fs::remove_all(localDir, ec);
    return HttpResponse{200, "application/json", picojson::value(body).serialize(), false};
}

std::map<std::string, std::string> loadConfigFileText(std::string const& text)
{
    return [&] {
        std::string err;
        auto const root = json::parse(text, &err);
        if (!err.empty() || !root.is<picojson::object>())
        {
            throw ConfigError("config body is not a JSON object");
        }
        std::map<std::string, std::string> out;
        for (auto const& [key, value] : root.get<picojson::object>())
        {
            if (value.is<std::string>())
            {
                out[key] = value.get<std::string>();
            }
            else if (value.is<bool>())
            {
                out[key] = value.get<bool>() ? "true" : "false";
            }
            else if (value.is<double>())
            {
                out[key] = std::to_string(static_cast<long long>(value.get<double>()));
            }
            else
            {
                out[key] = value.serialize();
            }
        }
        return out;
    }();
}
} // namespace mfa
