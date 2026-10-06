#include "config/config.hpp"
#include "config/store.hpp"
#include "fabric/domain.hpp"
#include "ops/httpserver.hpp"
#include "reconcile/controller.hpp"
#include "util/logging.hpp"
#include "util/net.hpp"
#include "version.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <thread>
#include <sys/resource.h>
#include <unistd.h>

extern char** environ;

namespace
{
volatile std::sig_atomic_t gStop = 0;

void onSignal(int)
{
    gStop = 1;
}

void onAlarm(int)
{
    std::_Exit(143);
}
} // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    // A mirror keeps one descriptor per grain (50 for 1 s at 50p), and eager mode
    // mirrors every flow of every peer. Docker's default soft limit of 1024 runs out
    // with a few dozen flows; mxlCreateFlowWriter then fails ("Too many open files").
    rlimit files{};
    if (getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur < files.rlim_max)
    {
        files.rlim_cur = files.rlim_max;
        setrlimit(RLIMIT_NOFILE, &files);
    }
    auto const env = mfa::environmentValues(environ);
    std::map<std::string, std::string> fileValues;
    auto const fileIt = env.find("AGENT_CONFIG_FILE");
    try
    {
        std::string configPath;
        if (fileIt != env.end() && !fileIt->second.empty())
        {
            configPath = fileIt->second;
        }
        else
        {
            auto const state = env.count("STATE_DIR") != 0 && !env.at("STATE_DIR").empty() ? env.at("STATE_DIR") : "/config";
            auto const candidate = (std::filesystem::path(state) / "agent.json").string();
            if (std::filesystem::is_regular_file(candidate))
            {
                configPath = candidate;
            }
        }
        if (!configPath.empty())
        {
            fileValues = mfa::loadConfigFile(configPath);
        }
        std::map<std::string, mfa::ValueOrigin> origin;
        auto cfg = mfa::loadLayered(fileValues, env, &origin);
        mfa::log::setLevel(cfg.log_level);
        if (cfg.config_file.empty())
        {
            std::error_code dirEc;
            std::filesystem::create_directories(cfg.state_dir, dirEc);
            if (!dirEc)
            {
                cfg.config_file = (std::filesystem::path(cfg.state_dir) / "agent.json").string();
            }
            else
            {
                mfa::log::warn("state_dir_unwritable", {{"path", cfg.state_dir}, {"error", dirEc.message()}});
            }
        }
        std::error_code ec;
        if (!std::filesystem::is_directory(cfg.mxl_root, ec))
        {
            mfa::log::error("mxl_root_missing", {{"path", cfg.mxl_root}});
            return 78;
        }
        if (!mfa::pathIsTmpfs(cfg.mxl_root))
        {
            mfa::log::warn("mxl_root_not_tmpfs", {{"path", cfg.mxl_root}});
        }
        auto const tai = mfa::taiOffsetSeconds();
        if (tai == 0)
        {
            mfa::log::warn("tai_offset_zero", {{"detail", "CLOCK_TAI offset is 0; grain indices will not match a disciplined facility"}});
        }
        auto scratch = std::filesystem::temp_directory_path() / "mfa-probe";
        std::filesystem::create_directories(scratch);
        mfa::ProviderProbe probe;
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            probe = mfa::probeFabrics(scratch.string());
            if (probe.ok)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250 * (1 << attempt)));
        }
        std::filesystem::remove_all(scratch, ec);
        bool const hasDefault = std::find(probe.providers.begin(), probe.providers.end(), cfg.default_provider) != probe.providers.end();
        bool const hasFallback = cfg.provider_fallback == "tcp" &&
                                  std::find(probe.providers.begin(), probe.providers.end(), std::string("tcp")) != probe.providers.end();
        if (!probe.ok || (!hasDefault && !hasFallback))
        {
            mfa::log::error("no_fabric_provider", {{"provider", cfg.default_provider}, {"error", probe.error}});
            return 75;
        }
        if (!hasDefault && hasFallback)
        {
            mfa::log::warn("provider_fallback_default", {{"from", cfg.default_provider}, {"to", "tcp"}});
        }
        mfa::log::info("startup", {{"version", mfa::kVersion}, {"host_id", cfg.host_id}, {"mxl", mfa::kMxlPin}});
        // 1.1.0 only: over verbs MXL's target keeps one receive for immediate data, and every paced
        // batch carries one, so pacing multiplied retransmissions. The setting is ignored now.
        if (char const* pacing = std::getenv("TRANSFER_PACING"); pacing != nullptr && std::string(pacing) != "off")
        {
            mfa::log::warn("transfer_pacing_removed", {{"value", pacing}, {"detail", "TRANSFER_PACING was removed in 1.2.0 and is ignored"}});
        }
        auto store = std::make_shared<mfa::ConfigStore>(cfg, origin, fileValues);
        mfa::Controller controller(store);
        mfa::HttpServer server;
        server.setHandler([&](mfa::HttpRequest const& request) { return controller.handle(request); });
        server.setSseHandler([&](mfa::SseEmit const& emit) { controller.stream(emit); });
        if (!server.start(cfg.web_port))
        {
            mfa::log::error("http_bind_failed", {{"port", std::to_string(cfg.web_port)}});
            return 75;
        }
        controller.start();
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        std::signal(SIGALRM, onAlarm);
        while (!gStop)
        {
            if (controller.fatal() != 0)
            {
                controller.stop();
                server.stop();
                return controller.fatal();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        alarm(static_cast<unsigned>(cfg.shutdown_timeout_s));
        controller.stop();
        server.stop();
        alarm(0);
        return 143;
    }
    catch (mfa::ConfigError const& ex)
    {
        std::cerr << "{\"level\":\"error\",\"event\":\"invalid_config\",\"error\":\"" << ex.what() << "\"}\n";
        return 78;
    }
    catch (std::exception const& ex)
    {
        std::cerr << "{\"level\":\"error\",\"event\":\"fatal\",\"error\":\"" << ex.what() << "\"}\n";
        return 75;
    }
}
