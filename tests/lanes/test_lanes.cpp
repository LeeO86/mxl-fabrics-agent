// Lane lifecycle against a real MXL domain (built when MXL is found; runs in the image build and in CI).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "fabric/qpcontrol.hpp"
#include "replication/engine.hpp"

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace mfa;
namespace fs = std::filesystem;

namespace
{
std::string const kFlowId = "5f1e0a52-7c41-4d6e-9b3a-1c2d3e4f5a6b";

std::string dataFlow(std::string const& id)
{
    return std::string(R"({"id":")") + id +
           R"(","label":"lane test","description":"temporary flow","format":"urn:x-nmos:format:data","media_type":"video/smpte291",)"
           R"("grain_rate":{"numerator":50,"denominator":1},"tags":{"urn:x-nmos:tag:grouphint/v1.0":["lane-test:Data"]}})";
}

struct TempDomain
{
    fs::path path;

    TempDomain()
    {
        path = fs::temp_directory_path() / ("mfa-lanes-" + std::to_string(::getpid()) + "-" + std::to_string(counter()++));
        fs::create_directories(path);
        std::ofstream(path / "domain_def.json") << R"({"id":"0b9b8f7e-1a2b-4c3d-8e9f-0a1b2c3d4e5f"})";
        std::ofstream(path / "options.json") << R"({"urn:x-mxl:option:history_duration/v1.0":200000000})";
    }

    ~TempDomain()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    static int& counter()
    {
        static int value = 0;
        return value;
    }
};

Config config()
{
    Config cfg;
    cfg.host_id = "lane-test";
    cfg.default_provider = "tcp";
    cfg.fabric_interface = "127.0.0.1";
    cfg.fabric_port_base = 29700;
    cfg.fabric_port_count = 50;
    return cfg;
}
} // namespace

TEST_CASE("libfabric gets the binary's rdma_cm hooks, and librdmacm's are behind them")
{
    // A toolchain or link change that drops the export (or the definitions) would leave the ACK timeout unset.
    CHECK(mfa::qp::hooksResolved());
}

TEST_CASE("a lane goes when its flow has nothing left")
{
    TempDomain domain;
    ReplicationEngine engine(config());
    std::string error;
    REQUIRE(engine.ensureWriter(domain.path.string(), dataFlow(kFlowId), &error));
    CHECK(engine.laneCount() == 1);
    engine.releaseWriter(domain.path.string(), kFlowId);
    CHECK(engine.laneCount() == 0);
    // Its final stats are handed out once, as retired.
    auto first = engine.takeLaneStats();
    CHECK(first.live.empty());
    CHECK(first.retired.size() == 1);
    CHECK(engine.takeLaneStats().retired.empty());
}

TEST_CASE("a failed source request leaves no lane")
{
    TempDomain domain;
    ReplicationEngine engine(config());
    PostRequest request;
    request.domain_id = "0b9b8f7e-1a2b-4c3d-8e9f-0a1b2c3d4e5f";
    request.flow_id = kFlowId; // no such origin flow
    request.dest_host_id = "peer";
    request.target_info = "{}";
    auto const result = engine.post(request, domain.path.string());
    CHECK(result.status == 500);
    CHECK(engine.laneCount() == 0);
}

TEST_CASE("a destination without its mirror writer leaves no lane after the sweep")
{
    TempDomain domain;
    ReplicationEngine engine(config());
    PullRequest pull;
    pull.domain_id = "0b9b8f7e-1a2b-4c3d-8e9f-0a1b2c3d4e5f";
    pull.flow_id = kFlowId;
    pull.flow_def_json = dataFlow(kFlowId);
    pull.mirror_path = domain.path.string();
    pull.peer = "peer";
    pull.control_url = "http://127.0.0.1:9/api/v1";
    pull.local_addr = "127.0.0.1";
    pull.provider = "tcp";
    engine.setPulls({pull}); // the target cannot be set up: no writer
    engine.sweepIdleLanes();
    CHECK(engine.laneCount() == 0);
    engine.setPulls({});
    CHECK(engine.laneCount() == 0);
}

TEST_CASE("the sweep never drops a lane that a caller is using")
{
    TempDomain domain;
    ReplicationEngine engine(config());
    std::atomic<bool> done{false};
    std::thread sweeper([&] {
        while (!done)
        {
            engine.sweepIdleLanes();
        }
    });
    int failures = 0;
    for (int i = 0; i < 300; ++i)
    {
        std::string error;
        if (!engine.ensureWriter(domain.path.string(), dataFlow(kFlowId), &error))
        {
            ++failures;
            break;
        }
        // The writer must be in the lane the engine hands out now: a lane dropped while it had the writer
        // would come back empty ("writer missing").
        std::uint64_t index = 0;
        if (!engine.domain(domain.path.string(), kFlowId)->commitPattern(kFlowId, &index, &error))
        {
            ++failures;
        }
        engine.releaseWriter(domain.path.string(), kFlowId);
    }
    done = true;
    sweeper.join();
    CHECK(failures == 0);
    engine.sweepIdleLanes();
    CHECK(engine.laneCount() == 0);
}
