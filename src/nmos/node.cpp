#include "nmos/node.hpp"

#include "util/logging.hpp"

#include <atomic>
#include <thread>

#if defined(MFA_WITH_NMOS)
#include "nmos/log_gate.h"
#include "nmos/model.h"
#include "nmos/node_resource.h"
#include "nmos/node_resources.h"
#include "nmos/node_server.h"
#include "nmos/server.h"
#endif

namespace mfa
{
struct NmosNode::Impl
{
#if defined(MFA_WITH_NMOS)
    std::thread thread;
    std::atomic<bool> run{false};
#endif
};

NmosNode::NmosNode(Config cfg, std::string nodeId, Ready ready)
    : cfg_(std::move(cfg))
    , nodeId_(std::move(nodeId))
    , ready_(std::move(ready))
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
            settings[U("label")] = web::json::value::string(utility::conversions::to_string_t(cfg_.host_id));
            settings[U("description")] = web::json::value::string(U("mxl-fabrics-agent"));
            settings[U("seed_id")] = web::json::value::string(utility::conversions::to_string_t(nodeId_));
            settings[U("service_name_prefix")] = web::json::value::string(U("mxl-fabrics-agent"));
            settings[U("logging_level")] = 20;
            settings[U("control_protocol_ws_port")] = -1;
            if (!cfg_.nmos_registry_address.empty())
            {
                settings[U("registry_address")] = web::json::value::string(utility::conversions::to_string_t(cfg_.nmos_registry_address));
                settings[U("registration_port")] = cfg_.nmos_registry_port;
            }
            nodeModel.settings = settings;
            nmos::insert_node_default_settings(nodeModel.settings);
            logModel.settings = nodeModel.settings;
            logModel.level = nmos::fields::logging_level(logModel.settings);

            auto implementation = nmos::experimental::node_implementation();
            auto server = nmos::experimental::make_node_server(nodeModel, implementation, logModel, gate);
            server.thread_functions.push_back([this, &nodeModel] {
                try
                {
                    {
                        auto lock = nodeModel.write_lock();
                        auto node = nmos::make_node(utility::conversions::to_string_t(nodeId_), nodeModel.settings);
                        std::string host = "127.0.0.1";
                        if (node.data.has_field(U("api")) && node.data.at(U("api")).has_field(U("endpoints")))
                        {
                            auto const& endpoints = node.data.at(U("api")).at(U("endpoints"));
                            if (endpoints.is_array() && endpoints.size() > 0 && endpoints.at(0).has_field(U("host")))
                            {
                                host = utility::conversions::to_utf8string(endpoints.at(0).at(U("host")).as_string());
                            }
                        }
                        web::json::value service;
                        service[U("href")] = web::json::value::string(utility::conversions::to_string_t(
                            "http://" + host + ":" + std::to_string(cfg_.web_port) + "/api/v1"));
                        service[U("type")] = web::json::value::string(U("urn:x-leeo86:service:mxl-fabrics-agent/v1.0"));
                        service[U("authorization")] = web::json::value::boolean(false);
                        node.data[U("services")] = web::json::value_of({service});
                        node.data[U("tags")][U("urn:x-leeo86:mxl-fabrics-agent:host-id")] =
                            web::json::value_of({web::json::value::string(utility::conversions::to_string_t(cfg_.host_id))});
                        if (!nmos::insert_resource(nodeModel.node_resources, std::move(node)).second)
                        {
                            throw std::runtime_error("failed to insert node");
                        }
                    }
                    if (ready_)
                    {
                        ready_(true, {});
                    }
                    auto lock = nodeModel.write_lock();
                    nodeModel.wait(lock, [&] { return nodeModel.shutdown || !impl_->run.load(); });
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
            log::info("nmos_node_started", {{"port", std::to_string(cfg_.nmos_port)}, {"node_id", nodeId_}});
            while (impl_->run.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
