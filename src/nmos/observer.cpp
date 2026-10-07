#include "nmos/observer.hpp"

#include "util/httpclient.hpp"
#include "util/jsonutil.hpp"
#include "util/logging.hpp"
#include "util/net.hpp"
#include "util/uuid.hpp"

#include <chrono>
#include <map>
#include <netinet/in.h>

#if __has_include(<dns_sd.h>)
#include <dns_sd.h>
#define MFA_HAS_DNSSD 1
#endif

namespace mfa
{
namespace
{
std::vector<picojson::value> asArray(std::string const& body)
{
    auto const root = json::parse(body);
    if (root.is<picojson::array>())
    {
        return root.get<picojson::array>();
    }
    return {};
}
} // namespace

NmosObserver::NmosObserver(Config cfg, LocalIdentity self, Wake wake)
    : cfg_(std::move(cfg))
    , self_(std::move(self))
    , wake_(std::move(wake))
{}

NmosObserver::~NmosObserver()
{
    stop();
}

void NmosObserver::updateConfig(Config const& cfg)
{
    std::lock_guard const lock{mu_};
    cfg_ = cfg;
    self_.extraNodeIds.clear();
    self_.extraHostnames.clear();
    for (auto const& id : cfg.local_node_ids)
    {
        self_.extraNodeIds.insert(id);
    }
    for (auto const& name : cfg.local_node_hostnames)
    {
        self_.extraHostnames.insert(name);
    }
    self_.cidrs = cfg.local_node_cidrs;
    self_.ownNodeId = nmosNodeId(cfg.nmos_seed, cfg.host_id);
    if (self_.cidrs.empty())
    {
        self_.resolveName = {};
    }
    else
    {
        self_.resolveName = [](std::string const& name) { return resolveNameCached(name); };
    }
}

void NmosObserver::setRegistered(bool ready)
{
    std::lock_guard const lock{mu_};
    snap_.registered = ready;
}

NmosSnapshot NmosObserver::snapshot() const
{
    std::lock_guard const lock{mu_};
    return snap_;
}

void NmosObserver::start()
{
    stop_ = false;
    thread_ = std::thread([this] { loop(); });
}

void NmosObserver::stop()
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

bool NmosObserver::discover(std::string& host, int& port)
{
#ifdef MFA_HAS_DNSSD
    DNSServiceRef browse = nullptr;
    struct Found
    {
        std::string host;
        int port = 0;
        bool done = false;
    } found;
    auto browseCb = [](DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType error, const char* name, const char* type,
                        const char* domain, void* ctx) {
        if (error != kDNSServiceErr_NoError || name == nullptr)
        {
            return;
        }
        auto* state = static_cast<Found*>(ctx);
        DNSServiceRef resolve = nullptr;
        struct Resolve
        {
            Found* found;
        } resolveCtx{state};
        auto resolveCb = [](DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType err, const char*, const char* hosttarget, uint16_t port,
                             uint16_t, const unsigned char*, void* raw) {
            if (err != kDNSServiceErr_NoError)
            {
                return;
            }
            auto* ctx = static_cast<Resolve*>(raw);
            ctx->found->host = hosttarget != nullptr ? hosttarget : "";
            if (!ctx->found->host.empty() && ctx->found->host.back() == '.')
            {
                ctx->found->host.pop_back();
            }
            ctx->found->port = ntohs(port);
            ctx->found->done = true;
        };
        if (DNSServiceResolve(&resolve, 0, 0, name, type, domain, resolveCb, &resolveCtx) == kDNSServiceErr_NoError)
        {
            DNSServiceProcessResult(resolve);
            DNSServiceRefDeallocate(resolve);
        }
    };
    if (DNSServiceBrowse(&browse, 0, 0, "_nmos-query._tcp", nullptr, browseCb, &found) != kDNSServiceErr_NoError)
    {
        return false;
    }
    DNSServiceProcessResult(browse);
    DNSServiceRefDeallocate(browse);
    if (!found.done || found.host.empty())
    {
        return false;
    }
    host = found.host;
    port = found.port != 0 ? found.port : 3211;
    return true;
#else
    (void)host;
    (void)port;
    return false;
#endif
}

void NmosObserver::poll(std::string const& base)
{
    auto const nodes = httpRequest("GET", base + "/nodes");
    auto const devices = httpRequest("GET", base + "/devices");
    auto const receivers = httpRequest("GET", base + "/receivers");
    NmosSnapshot next;
    {
        std::lock_guard const lock{mu_};
        next.registered = snap_.registered;
        next.poll_errors = snap_.poll_errors;
        next.registry = base;
    }
    if (nodes.status != 200 || devices.status != 200 || receivers.status != 200)
    {
        next.registry_up = false;
        next.error = "query api";
        next.poll_errors += 1;
        std::lock_guard const lock{mu_};
        snap_ = next;
        return;
    }
    next.registry_up = true;
    std::map<std::string, NodeView> nodeById;
    std::map<std::string, std::string> deviceNode;
    std::map<std::string, std::string> deviceControl;
    for (auto const& item : asArray(nodes.body))
    {
        if (!item.is<picojson::object>())
        {
            continue;
        }
        auto const& obj = item.get<picojson::object>();
        NodeView node;
        node.id = json::asString(obj, "id").value_or("");
        node.hostname = json::asString(obj, "hostname").value_or("");
        node.label = json::asString(obj, "label").value_or("");
        if (obj.count("api") && obj.at("api").is<picojson::object>())
        {
            auto const& api = obj.at("api").get<picojson::object>();
            if (api.count("endpoints") && api.at("endpoints").is<picojson::array>())
            {
                for (auto const& ep : api.at("endpoints").get<picojson::array>())
                {
                    if (!ep.is<picojson::object>())
                    {
                        continue;
                    }
                    EndpointRef ref;
                    ref.host = json::asString(ep.get<picojson::object>(), "host").value_or("");
                    ref.port = static_cast<int>(json::asNumber(ep.get<picojson::object>(), "port").value_or(0));
                    node.endpoints.push_back(ref);
                }
            }
        }
        if (obj.count("services") && obj.at("services").is<picojson::array>())
        {
            for (auto const& service : obj.at("services").get<picojson::array>())
            {
                if (!service.is<picojson::object>())
                {
                    continue;
                }
                auto const type = json::asString(service.get<picojson::object>(), "type").value_or("");
                if (type == "urn:x-leeo86:service:mxl-fabrics-agent/v1.0")
                {
                    PeerSnapshot agent;
                    agent.host_id = node.hostname;
                    agent.control_url = json::asString(service.get<picojson::object>(), "href").value_or("");
                    if (obj.count("tags") && obj.at("tags").is<picojson::object>())
                    {
                        auto const& tags = obj.at("tags").get<picojson::object>();
                        auto const tag = tags.find("urn:x-leeo86:mxl-fabrics-agent:host-id");
                        if (tag != tags.end() && tag->second.is<picojson::array>() && !tag->second.get<picojson::array>().empty() &&
                            tag->second.get<picojson::array>().front().is<std::string>())
                        {
                            agent.host_id = tag->second.get<picojson::array>().front().get<std::string>();
                        }
                    }
                    if (agent.host_id != self_.ownNodeId && !agent.control_url.empty())
                    {
                        next.agents.push_back(std::move(agent));
                    }
                }
            }
        }
        nodeById[node.id] = node;
    }
    for (auto const& item : asArray(devices.body))
    {
        if (!item.is<picojson::object>())
        {
            continue;
        }
        auto const& obj = item.get<picojson::object>();
        auto const id = json::asString(obj, "id").value_or("");
        deviceNode[id] = json::asString(obj, "node_id").value_or("");
        if (obj.count("controls") && obj.at("controls").is<picojson::array>())
        {
            for (auto const& control : obj.at("controls").get<picojson::array>())
            {
                if (!control.is<picojson::object>())
                {
                    continue;
                }
                auto const type = json::asString(control.get<picojson::object>(), "type").value_or("");
                if (type.find("urn:x-nmos:control:sr-ctrl/v1.2") != std::string::npos || deviceControl.count(id) == 0)
                {
                    if (type.find("sr-ctrl") != std::string::npos)
                    {
                        deviceControl[id] = json::asString(control.get<picojson::object>(), "href").value_or("");
                    }
                }
            }
        }
    }
    LocalIdentity self;
    {
        std::lock_guard const lock{mu_};
        self = self_;
    }
    next.registered = nodeById.count(self.ownNodeId) != 0;
    for (auto const& item : asArray(receivers.body))
    {
        if (!item.is<picojson::object>())
        {
            continue;
        }
        auto const& obj = item.get<picojson::object>();
        auto const transport = json::asString(obj, "transport").value_or("");
        if (transport != "urn:x-nmos:transport:mxl")
        {
            continue;
        }
        ReceiverView receiver;
        receiver.id = json::asString(obj, "id").value_or("");
        receiver.label = json::asString(obj, "label").value_or(receiver.id);
        receiver.device_id = json::asString(obj, "device_id").value_or("");
        auto const nodeId = deviceNode[receiver.device_id];
        receiver.node_id = nodeId;
        auto const node = nodeById.find(nodeId);
        if (node == nodeById.end())
        {
            continue;
        }
        auto const decision = matchLocalNode(node->second, self);
        if (!decision.local)
        {
            continue;
        }
        log::debug("nmos_node_local", {{"node_id", node->second.id}, {"hostname", node->second.hostname}, {"rule", decision.rule}});
        receiver.node_label = node->second.label;
        receiver.control_href = deviceControl[receiver.device_id];
        if (!receiver.control_href.empty())
        {
            auto const url = receiverActiveUrl(receiver.control_href, receiver.id);
            auto const active = httpRequest("GET", url);
            auto parsed = active.status == 200 ? parseActive(active.body) : std::nullopt;
            if (parsed)
            {
                receiver.active = *parsed;
                activeFailed_.erase(receiver.id);
            }
            else if (activeFailed_.insert(receiver.id).second)
            {
                // Without /active the receiver counts as not routed: no demand, no replication.
                log::warn("nmos_active_unavailable", {{"receiver_id", receiver.id}, {"url", url}, {"status", std::to_string(active.status)}});
            }
        }
        next.receivers.push_back(std::move(receiver));
    }
    {
        std::lock_guard const lock{mu_};
        snap_ = std::move(next);
    }
    if (wake_)
    {
        wake_();
    }
}

void NmosObserver::loop()
{
    while (true)
    {
        Config cfg;
        {
            std::lock_guard const lock{mu_};
            if (stop_)
            {
                return;
            }
            cfg = cfg_;
        }
        if (!cfg.nmos_enable)
        {
            std::lock_guard const lock{mu_};
            snap_.registry = "disabled";
            snap_.registry_up = true;
        }
        else
        {
            std::string host = cfg.queryHost();
            int port = cfg.nmos_query_port;
            if (host.empty())
            {
                if (!cfg.nmos_dns_sd)
                {
                    std::lock_guard const lock{mu_};
                    snap_.registry_up = false;
                    snap_.registry = "unconfigured";
                    snap_.error.clear();
                }
                else if (!discover(host, port))
                {
                    std::lock_guard const lock{mu_};
                    snap_.registry_up = false;
                    snap_.registry = "dns-sd";
                    snap_.error = "query service not discovered";
                    snap_.poll_errors += 1;
                }
            }
            if (!host.empty())
            {
                poll("http://" + host + ":" + std::to_string(port) + "/x-nmos/query/v1.3");
            }
        }
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.nmos_poll_interval_ms);
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
