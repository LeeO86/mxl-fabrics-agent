#include "reconcile/controller.hpp"

#include "mirror/lifecycle.hpp"
#include "util/httpclient.hpp"
#include "util/jsonutil.hpp"
#include "util/logging.hpp"
#include "util/net.hpp"
#include "util/uuid.hpp"
#include "version.hpp"

#include <sys/statvfs.h>

#include <algorithm>
#include <filesystem>
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

std::string domainDef(std::string const& domainId, std::string const& sourceHost, std::string const& owner)
{
    picojson::object marker;
    marker["mirror"] = picojson::value(true);
    marker["source_host_id"] = picojson::value(sourceHost);
    marker["owner_host_id"] = picojson::value(owner);
    picojson::object obj;
    obj["id"] = picojson::value(domainId);
    obj["label"] = picojson::value("mirror " + domainId);
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

LocalIdentity identityFrom(Config const& cfg)
{
    LocalIdentity id;
    id.hostname = localHostname();
    id.addresses = localAddresses();
    id.extraNodeIds.insert(cfg.local_node_ids.begin(), cfg.local_node_ids.end());
    id.extraHostnames.insert(cfg.local_node_hostnames.begin(), cfg.local_node_hostnames.end());
    id.ownNodeId = nodeIdForHost(cfg.host_id).str();
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
}

Controller::~Controller()
{
    stop();
}

void Controller::start()
{
    auto const cfg = store_->get();
    if (cfg.nmos_enable)
    {
        node_ = std::make_unique<NmosNode>(cfg, nodeIdForHost(cfg.host_id).str(), [this](bool ok, std::string const& error) {
            nmosReady_ = ok;
            nmosError_ = error;
            observer_.setRegistered(ok);
            cv_.notify_all();
        });
        node_->start();
    }
    scanner_.start();
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
            }
        }
    }
    observer_.stop();
    peers_.stop();
    scanner_.stop();
    if (node_)
    {
        node_->stop();
    }
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
    scanner_.setPeerDomains(peerDomains);
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
    auto const free = freeMb(cfg.mxl_root);
    for (auto const& remote : remotes)
    {
        auto const key = remote.domain->domain_id + "/" + remote.flow->flow_id;
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
        if (plan.remove && exists)
        {
            engine_.releaseWriter(path, remote.flow->flow_id);
            std::error_code ec;
            fs::remove_all(fs::path(path) / (remote.flow->flow_id + ".mxl-flow"), ec);
            log::info("mirror_removed", {{"flow_id", remote.flow->flow_id}, {"domain_id", remote.domain->domain_id}});
        }
        if (plan.create)
        {
            writeText(fs::path(path) / "domain_def.json", domainDef(remote.domain->domain_id, remote.peer->host_id, cfg.host_id));
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
            pulls.push_back(std::move(pull));
        }
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
    {
        std::lock_guard const lock{mu_};
        demand_ = std::move(demand);
        mirrors_ = std::move(mirrorViews);
    }
    renderMetrics();
    publish("inventory", std::to_string(inventory.revision));
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
    metrics_.setGauge("flows", originActive, {{"kind", "origin"}, {"state", "active"}});
    metrics_.setGauge("flows", originIdle, {{"kind", "origin"}, {"state", "inactive"}});
    struct statvfs st{};
    if (statvfs(cfg.mxl_root.c_str(), &st) == 0)
    {
        metrics_.setGauge("tmpfs_free_bytes", static_cast<double>(st.f_bavail) * static_cast<double>(st.f_frsize));
        metrics_.setGauge("tmpfs_size_bytes", static_cast<double>(st.f_blocks) * static_cast<double>(st.f_frsize));
    }
    int up = 0;
    for (auto const& peer : peerSnap)
    {
        if (peer.up)
        {
            ++up;
        }
        metrics_.setGauge("peer_up", peer.up ? 1 : 0, {{"peer", peer.host_id}});
    }
    metrics_.setGauge("peers_up", up);
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
    for (auto const& row : engine_.status())
    {
        if (row.state == "active" || row.state == "pending")
        {
            reps[row.role + "/" + row.provider] += 1;
        }
        if (cfg.metrics_per_flow)
        {
            metrics_.setGauge("replication_lag_grains", static_cast<double>(row.lag), {{"flow_id", row.flow_id}, {"peer", row.peer}});
            metrics_.setGauge("completion_queue_depth", row.cq_depth, {{"flow_id", row.flow_id}});
            metrics_.addCounter("replication_grains_total", 0, {{"flow_id", row.flow_id}, {"peer", row.peer}, {"role", row.role}});
        }
    }
    for (auto const& [key, count] : reps)
    {
        auto const slash = key.find('/');
        metrics_.setGauge("replications_active", count, {{"role", key.substr(0, slash)}, {"provider", key.substr(slash + 1)}});
    }
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
        bool const nmosOk = !cfg.nmos_enable || nmosReady_.load();
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
        return json(200, scanner_.snapshot().canonicalJson());
    }
    if (request.method == "GET" && request.path == "/api/v1/mirrors")
    {
        std::lock_guard const lock{mu_};
        picojson::array arr;
        for (auto const& mirror : mirrors_)
        {
            picojson::object obj;
            obj["domain_id"] = picojson::value(mirror.domain_id);
            obj["path"] = picojson::value(mirror.path);
            obj["source_host_id"] = picojson::value(mirror.source_host_id);
            obj["state"] = picojson::value(mirror.options_json);
            picojson::array flows;
            for (auto const& flow : mirror.flows)
            {
                picojson::object f;
                f["flow_id"] = picojson::value(flow.flow_id);
                f["media_type"] = picojson::value(flow.media_type);
                f["format"] = picojson::value(flow.format);
                f["active"] = picojson::value(flow.active);
                flows.push_back(picojson::value(f));
            }
            obj["flows"] = picojson::value(flows);
            arr.push_back(picojson::value(obj));
        }
        return json(200, picojson::value(arr).serialize());
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
        picojson::array arr;
        for (auto const& peer : peers_.snapshot())
        {
            picojson::object obj;
            obj["host_id"] = picojson::value(peer.host_id);
            obj["control_url"] = picojson::value(peer.control_url);
            obj["up"] = picojson::value(peer.up);
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
