#include "nmos/node.hpp"

#include "util/httpclient.hpp"
#include "util/logging.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <thread>

#if defined(MFA_WITH_NMOS)
#include "nmos/log_gate.h"
#include "nmos/model.h"
#include "nmos/node_resource.h"
#include "nmos/node_resources.h"
#include "nmos/node_server.h"
#include "nmos/resources.h"
#include "nmos/server.h"
#endif

namespace mfa
{
struct NmosNode::Impl
{
#if defined(MFA_WITH_NMOS)
    std::thread thread;
    std::atomic<bool> run{false};
    std::atomic<bool> everRegistered{false};
    std::atomic<bool> deregistered{false};
#endif
};

NmosNode::NmosNode(Config cfg, std::string nodeId, Ready ready, Registered registered)
    : cfg_(std::move(cfg))
    , nodeId_(std::move(nodeId))
    , ready_(std::move(ready))
    , registered_(std::move(registered))
    , impl_(new Impl)
{}

NmosNode::~NmosNode()
{
    stop();
    delete impl_;
}

void NmosNode::start()
{
#if !defined(MFA_WITH_NMOS)
    if (ready_)
    {
        ready_(false, "built without nmos-cpp");
    }
    return;
#else
    impl_->run = true;
    impl_->thread = std::thread([this] {
        nmos::experimental::log_model logModel;
        std::ostream errorLog(std::cerr.rdbuf());
        std::filebuf discarded;
        std::ostream accessLog(&discarded);
        nmos::experimental::log_gate gate(errorLog, accessLog, logModel);
        try
        {
            nmos::node_model nodeModel;
            web::json::value settings = web::json::value::object();
            settings[U("http_port")] = cfg_.nmos_port;
            settings[U("label")] = web::json::value::string(utility::conversions::to_string_t(cfg_.nmos_label));
            settings[U("description")] = web::json::value::string(U("mxl-fabrics-agent"));
            settings[U("seed_id")] = web::json::value::string(utility::conversions::to_string_t(nodeId_));
            settings[U("host_name")] = web::json::value::string(utility::conversions::to_string_t(cfg_.nmos_host_address));
            settings[U("host_address")] = web::json::value::string(utility::conversions::to_string_t(cfg_.nmos_host_address));
            web::json::value addresses = web::json::value::array();
            addresses[0] = web::json::value::string(utility::conversions::to_string_t(cfg_.nmos_host_address));
            settings[U("host_addresses")] = addresses;
            settings[U("service_name_prefix")] = web::json::value::string(U("mxl-fabrics-agent"));
            settings[U("logging_level")] = 20;
            settings[U("control_protocol_ws_port")] = -1;
            // pri and highest_pri at INT_MAX disable mDNS advertisement and DNS-SD browse.
            // registry_address alone is only a fallback after a browse.
            if (!cfg_.nmos_dns_sd)
            {
                int const off = std::numeric_limits<int>::max();
                settings[U("pri")] = off;
                settings[U("highest_pri")] = off;
            }
            if (!cfg_.nmos_registry_address.empty())
            {
                settings[U("registry_address")] = web::json::value::string(utility::conversions::to_string_t(cfg_.nmos_registry_address));
                settings[U("registration_port")] = cfg_.nmos_registry_port;
            }
            nodeModel.settings = settings;
            nmos::insert_node_default_settings(nodeModel.settings);
            if (!cfg_.nmos_dns_sd)
            {
                int const off = std::numeric_limits<int>::max();
                nodeModel.settings[U("pri")] = off;
                nodeModel.settings[U("highest_pri")] = off;
            }
            nodeModel.settings[U("host_address")] = web::json::value::string(utility::conversions::to_string_t(cfg_.nmos_host_address));
            nodeModel.settings[U("host_addresses")] = addresses;
            nodeModel.settings[U("host_name")] = web::json::value::string(utility::conversions::to_string_t(cfg_.nmos_host_address));
            logModel.settings = nodeModel.settings;
            logModel.level = nmos::fields::logging_level(logModel.settings);

            auto implementation = nmos::experimental::node_implementation();
            implementation.on_registration_changed([this](web::uri const& uri) {
                if (uri.is_empty())
                {
                    impl_->deregistered = true;
                    if (registered_)
                    {
                        registered_(false);
                    }
                    return;
                }
                impl_->everRegistered = true;
                impl_->deregistered = false;
                if (registered_)
                {
                    registered_(true);
                }
            });
            auto server = nmos::experimental::make_node_server(nodeModel, implementation, logModel, gate);
            server.thread_functions.push_back([this, &nodeModel] {
                try
                {
                    {
                        auto lock = nodeModel.write_lock();
                        auto node = nmos::make_node(utility::conversions::to_string_t(nodeId_), nodeModel.settings);
                        web::json::value service;
                        service[U("href")] = web::json::value::string(utility::conversions::to_string_t(
                            "http://" + cfg_.nmos_host_address + ":" + std::to_string(cfg_.web_port) + "/api/v1"));
                        service[U("type")] = web::json::value::string(U("urn:x-leeo86:service:mxl-fabrics-agent/v1.0"));
                        service[U("authorization")] = web::json::value::boolean(false);
                        node.data[U("services")] = web::json::value_of({service});
                        web::json::value tags = web::json::value::object();
                        if (node.data.has_field(U("tags")) && node.data.at(U("tags")).is_object())
                        {
                            tags = node.data.at(U("tags"));
                        }
                        for (auto const& [key, values] : cfg_.nmos_tags)
                        {
                            web::json::value arr = web::json::value::array();
                            for (std::size_t i = 0; i < values.size(); ++i)
                            {
                                arr[i] = web::json::value::string(utility::conversions::to_string_t(values[i]));
                            }
                            tags[utility::conversions::to_string_t(key)] = arr;
                        }
                        tags[U("urn:x-leeo86:mxl-fabrics-agent:host-id")] =
                            web::json::value_of({web::json::value::string(utility::conversions::to_string_t(cfg_.host_id))});
                        node.data[U("tags")] = tags;
                        node.data[U("label")] = web::json::value::string(utility::conversions::to_string_t(cfg_.nmos_label));
                        if (node.data.has_field(U("api")) && node.data.at(U("api")).has_field(U("endpoints")) &&
                            node.data.at(U("api")).at(U("endpoints")).is_array())
                        {
                            auto& endpoints = node.data[U("api")][U("endpoints")];
                            for (std::size_t i = 0; i < endpoints.size(); ++i)
                            {
                                endpoints[i][U("host")] = web::json::value::string(utility::conversions::to_string_t(cfg_.nmos_host_address));
                            }
                        }
                        if (!nmos::insert_resource(nodeModel.node_resources, std::move(node)).second)
                        {
                            throw std::runtime_error("failed to insert node");
                        }
                        // The registration thread waits on this condition. Without the
                        // notify it never sees the node and /readyz stays down.
                        nodeModel.notify();
                    }
                    if (ready_)
                    {
                        ready_(true, {});
                    }
                    auto lock = nodeModel.write_lock();
                    nodeModel.wait(lock, [&] { return !impl_->run.load(); });
                }
                catch (std::exception const& ex)
                {
                    if (ready_)
                    {
                        ready_(false, ex.what());
                    }
                }
            });
            nmos::server_guard guard(server);
            log::info("nmos_node_started",
                {{"port", std::to_string(cfg_.nmos_port)},
                    {"node_id", nodeId_},
                    {"host_address", cfg_.nmos_host_address},
                    {"dns_sd", cfg_.nmos_dns_sd ? "true" : "false"}});
            while (impl_->run.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            {
                auto lock = nodeModel.write_lock();
                nmos::erase_resource(nodeModel.node_resources, utility::conversions::to_string_t(nodeId_), true);
                nodeModel.notify();
            }
            if (cfg_.registryConfigured() && impl_->everRegistered.load())
            {
                int const seconds = std::min(2, std::max(1, cfg_.shutdown_timeout_s - 2));
                auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
                while (!impl_->deregistered.load() && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
            if (!cfg_.nmos_registry_address.empty())
            {
                auto const url = "http://" + cfg_.nmos_registry_address + ":" + std::to_string(cfg_.nmos_registry_port) +
                                  "/x-nmos/registration/v1.3/resource/nodes/" + nodeId_;
                auto const deleted = httpRequest("DELETE", url, {}, std::chrono::milliseconds(1500));
                log::info("nmos_node_delete", {{"node_id", nodeId_}, {"status", std::to_string(deleted.status)}, {"error", deleted.error}});
            }
            {
                auto lock = nodeModel.write_lock();
                nodeModel.shutdown = true;
                nodeModel.notify();
            }
        }
        catch (std::exception const& ex)
        {
            log::error("nmos_node_failed", {{"error", ex.what()}});
            if (ready_)
            {
                ready_(false, ex.what());
            }
        }
    });
#endif
}

void NmosNode::stop()
{
#if defined(MFA_WITH_NMOS)
    if (impl_ != nullptr)
    {
        impl_->run = false;
        if (impl_->thread.joinable())
        {
            impl_->thread.join();
        }
    }
#endif
}
} // namespace mfa
