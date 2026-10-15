#include "peer/manager.hpp"

#include "util/httpclient.hpp"
#include "util/jsonutil.hpp"
#include "util/logging.hpp"

#include <algorithm>
#include <chrono>

namespace mfa
{
Inventory parseInventoryJson(std::string const& body)
{
    Inventory inventory;
    auto const root = json::parse(body);
    if (!root.is<picojson::object>())
    {
        return inventory;
    }
    auto const& obj = root.get<picojson::object>();
    inventory.host_id = json::asString(obj, "host_id").value_or("");
    inventory.revision = static_cast<std::uint64_t>(json::asNumber(obj, "revision").value_or(0));
    auto const it = obj.find("domains");
    if (it == obj.end() || !it->second.is<picojson::array>())
    {
        return inventory;
    }
    for (auto const& item : it->second.get<picojson::array>())
    {
        if (!item.is<picojson::object>())
        {
            continue;
        }
        auto const& domainObj = item.get<picojson::object>();
        DomainRecord domain;
        domain.domain_id = json::asString(domainObj, "domain_id").value_or("");
        domain.path = json::asString(domainObj, "path").value_or("");
        domain.kind = "local";
        if (domainObj.count("options"))
        {
            domain.options_json = domainObj.at("options").serialize();
        }
        auto const flows = domainObj.find("flows");
        if (flows != domainObj.end() && flows->second.is<picojson::array>())
        {
            for (auto const& flowItem : flows->second.get<picojson::array>())
            {
                if (!flowItem.is<picojson::object>())
                {
                    continue;
                }
                auto const& flowObj = flowItem.get<picojson::object>();
                FlowRecord flow;
                flow.flow_id = json::asString(flowObj, "flow_id").value_or("");
                flow.format = json::asString(flowObj, "format").value_or("discrete");
                flow.media_type = json::asString(flowObj, "media_type").value_or("");
                flow.active = json::asBool(flowObj, "active").value_or(false);
                flow.live = json::asBool(flowObj, "live").value_or(false);
                if (auto const head = json::asNumber(flowObj, "head_index"))
                {
                    flow.head_index = static_cast<std::uint64_t>(*head);
                }
                if (flowObj.count("flow_def"))
                {
                    flow.flow_def_json = flowObj.at("flow_def").serialize();
                }
                flow.ring_depth = static_cast<std::uint64_t>(json::asNumber(flowObj, "ring_depth").value_or(0));
                flow.payload_size = static_cast<std::uint64_t>(json::asNumber(flowObj, "payload_size").value_or(0));
                if (flowObj.count("grain_rate") && flowObj.at("grain_rate").is<picojson::object>())
                {
                    auto const& rate = flowObj.at("grain_rate").get<picojson::object>();
                    flow.grain_rate_num = static_cast<std::int64_t>(json::asNumber(rate, "numerator").value_or(0));
                    flow.grain_rate_den = static_cast<std::int64_t>(json::asNumber(rate, "denominator").value_or(1));
                }
                domain.flows.push_back(std::move(flow));
            }
        }
        inventory.domains.push_back(std::move(domain));
    }
    return inventory;
}

std::uint32_t nextPollCount(PeerSnapshot const* previous, PeerSnapshot const& now)
{
    if (!now.up)
    {
        return 0;
    }
    if (previous == nullptr || !previous->up || previous->boot_id != now.boot_id)
    {
        return 1;
    }
    return previous->polls + 1;
}

bool inventorySettled(PeerSnapshot const& peer)
{
    return peer.up && peer.inventory.revision > 0 && peer.polls >= 2;
}

void InventorySettleTracker::update(std::vector<PeerSnapshot> const& peers, std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration limit)
{
    std::map<std::string, InventoryView> views;
    std::map<std::string, std::chrono::steady_clock::time_point> since;
    std::map<std::string, std::chrono::steady_clock::time_point> down;
    for (auto const& peer : peers)
    {
        if (inventorySettled(peer))
        {
            views[peer.host_id] = InventoryView::Settled;
            continue;
        }
        auto const previous = unsettledSince_.find(peer.host_id);
        if (!peer.up)
        {
            views[peer.host_id] = InventoryView::Down;
            auto const wasDown = downSince_.find(peer.host_id);
            auto const downAt = wasDown != downSince_.end() ? wasDown->second : now;
            down[peer.host_id] = downAt;
            // Down in between does not reset the wait (a flapping peer); down for longer than the limit does.
            if (previous != unsettledSince_.end() && now - downAt < limit)
            {
                since[peer.host_id] = previous->second;
            }
            continue;
        }
        auto const start = previous != unsettledSince_.end() ? previous->second : now;
        since[peer.host_id] = start;
        views[peer.host_id] = now - start < limit ? InventoryView::Pending : InventoryView::Down;
    }
    views_ = std::move(views);
    unsettledSince_ = std::move(since);
    downSince_ = std::move(down);
}

InventoryView InventorySettleTracker::view(std::string const& hostId) const
{
    auto const it = views_.find(hostId);
    return it != views_.end() ? it->second : InventoryView::Down;
}

bool InventorySettleTracker::anyPending() const
{
    return std::any_of(views_.begin(), views_.end(), [](auto const& item) { return item.second == InventoryView::Pending; });
}

bool InventorySettleTracker::settledFor(std::string const& sourceHost, std::set<std::string> const& holders) const
{
    if (view(sourceHost) != InventoryView::Settled)
    {
        return false;
    }
    return std::none_of(holders.begin(), holders.end(), [&](std::string const& host) { return view(host) == InventoryView::Pending; });
}

PeerManager::PeerManager(Config cfg, Wake wake)
    : cfg_(std::move(cfg))
    , wake_(std::move(wake))
{}

PeerManager::~PeerManager()
{
    stop();
}

void PeerManager::updateConfig(Config const& cfg)
{
    std::lock_guard const lock{mu_};
    cfg_ = cfg;
}

void PeerManager::setDiscovered(std::vector<PeerSnapshot> const& discovered)
{
    std::lock_guard const lock{mu_};
    discovered_ = discovered;
}

std::vector<PeerSnapshot> PeerManager::snapshot() const
{
    std::lock_guard const lock{mu_};
    return live_;
}

void PeerManager::start()
{
    stop_ = false;
    thread_ = std::thread([this] { loop(); });
}

void PeerManager::stop()
{
    {
        std::lock_guard const lock{mu_};
        stop_ = true;
    }
    if (thread_.joinable())
    {
        thread_.join();
    }
}

std::vector<PeerSnapshot> PeerManager::mergeUnlocked() const
{
    std::vector<PeerSnapshot> peers = discovered_;
    for (auto const& configured : cfg_.peers)
    {
        bool found = false;
        for (auto& peer : peers)
        {
            if (peer.host_id == configured.host_id)
            {
                if (!configured.control_url.empty())
                {
                    peer.control_url = configured.control_url;
                }
                peer.provider = configured.provider;
                peer.local_addr = configured.local_fabric_addr;
                peer.remote_addr = configured.remote_fabric_addr;
                found = true;
                break;
            }
        }
        if (!found)
        {
            PeerSnapshot peer;
            peer.host_id = configured.host_id;
            peer.control_url = configured.control_url;
            peer.provider = configured.provider;
            peer.local_addr = configured.local_fabric_addr;
            peer.remote_addr = configured.remote_fabric_addr;
            peers.push_back(std::move(peer));
        }
    }
    return peers;
}

void PeerManager::loop()
{
    while (true)
    {
        Config cfg;
        std::vector<PeerSnapshot> peers;
        {
            std::lock_guard const lock{mu_};
            if (stop_)
            {
                return;
            }
            cfg = cfg_;
            peers = mergeUnlocked();
        }
        for (auto& peer : peers)
        {
            if (peer.host_id == cfg.host_id || peer.control_url.empty())
            {
                peer.up = false;
                peer.error = peer.control_url.empty() ? "no control url" : "";
                continue;
            }
            auto const info = httpRequest("GET", peer.control_url + "/info");
            auto const inventory = httpRequest("GET", peer.control_url + "/inventory");
            if (info.status != 200 || inventory.status != 200)
            {
                peer.up = false;
                peer.error = info.error.empty() ? inventory.error : info.error;
                if (peer.error.empty())
                {
                    peer.error = "http " + std::to_string(info.status);
                }
                continue;
            }
            peer.up = true;
            peer.error.clear();
            auto const infoJson = json::parse(info.body);
            if (infoJson.is<picojson::object>())
            {
                peer.boot_id = json::asString(infoJson.get<picojson::object>(), "boot_id").value_or("");
            }
            peer.inventory = parseInventoryJson(inventory.body);
            peer.revision = peer.inventory.revision;
        }
        bool changed = false;
        {
            std::lock_guard const lock{mu_};
            if (peers.size() != live_.size())
            {
                changed = true;
            }
            for (auto& peer : peers)
            {
                auto const previous = std::find_if(live_.begin(), live_.end(), [&](auto const& p) { return p.host_id == peer.host_id; });
                peer.polls = nextPollCount(previous != live_.end() ? &*previous : nullptr, peer);
                // The poll that settles the inventory: the controller may now judge flows the peer does not list.
                if (peer.polls == 2)
                {
                    changed = true;
                }
            }
            live_ = std::move(peers);
        }
        if (changed && wake_)
        {
            wake_();
        }
        auto const step = std::chrono::milliseconds(cfg.peer_poll_interval_ms);
        auto const deadline = std::chrono::steady_clock::now() + step;
        while (std::chrono::steady_clock::now() < deadline)
        {
            {
                std::lock_guard const lock{mu_};
                if (stop_)
                {
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}
} // namespace mfa
