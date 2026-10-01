#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace
{
void writeFile(std::filesystem::path const& path, std::string const& body)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << body;
}
} // namespace

int readFlow(std::string const& domain, std::string const& flowId, int seconds)
{
    auto* instance = mxlCreateInstance(domain.c_str(), "");
    if (instance == nullptr)
    {
        return 1;
    }
    mxlFlowReader reader = nullptr;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (reader == nullptr && mxlCreateFlowReader(instance, flowId.c_str(), nullptr, &reader) != MXL_STATUS_OK)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        mxlFlowRuntimeInfo runtime{};
        if (mxlFlowReaderGetRuntimeInfo(reader, &runtime) == MXL_STATUS_OK && runtime.headIndex != 0 && runtime.headIndex != UINT64_MAX)
        {
            mxlGrainInfo info{};
            std::uint8_t* payload = nullptr;
            if (mxlFlowReaderGetGrainNonBlocking(reader, runtime.headIndex, &info, &payload) == MXL_STATUS_OK)
            {
                std::uint64_t stamped = 0;
                if (payload != nullptr && info.grainSize >= sizeof(stamped))
                {
                    std::memcpy(&stamped, payload, sizeof(stamped));
                }
                std::cout << "head=" << runtime.headIndex << " grain=" << info.index << " stamped=" << stamped << "\n";
                if (info.index == runtime.headIndex && stamped == info.index)
                {
                    mxlReleaseFlowReader(instance, reader);
                    mxlDestroyInstance(instance);
                    return 0;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (reader != nullptr)
    {
        mxlReleaseFlowReader(instance, reader);
    }
    mxlDestroyInstance(instance);
    return 1;
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "read")
    {
        return readFlow(argc > 2 ? argv[2] : "/dev/shm/mxl-b/mirror", argc > 3 ? argv[3] : "", argc > 4 ? std::atoi(argv[4]) : 20);
    }
    std::string domain = argc > 1 ? argv[1] : "/dev/shm/mxl-a/src";
    std::string flowId = argc > 2 ? argv[2] : "11111111-1111-4111-8111-111111111111";
    int frames = argc > 3 ? std::atoi(argv[3]) : 100;
    std::filesystem::create_directories(domain);
    writeFile(std::filesystem::path(domain) / "options.json", "{\"urn:x-mxl:option:history_duration/v1.0\":1000000000}");
    writeFile(std::filesystem::path(domain) / "domain_def.json", std::string("{\"id\":\"") + "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa" + "\"}");
    std::string def = std::string(R"({
  "id": ")") + flowId +
                      R"(",
  "label": "pattern",
  "description": "integration writer",
  "format": "urn:x-nmos:format:data",
  "media_type": "video/smpte291",
  "grain_rate": {"numerator": 25, "denominator": 1},
  "tags": {"urn:x-nmos:tag:grouphint/v1.0": ["pattern:Data"]}
})";
    auto* instance = mxlCreateInstance(domain.c_str(), "");
    if (instance == nullptr)
    {
        std::cerr << "mxlCreateInstance failed\n";
        return 1;
    }
    mxlFlowWriter writer = nullptr;
    mxlFlowConfigInfo info{};
    bool created = false;
    if (mxlCreateFlowWriter(instance, def.c_str(), nullptr, &writer, &info, &created) != MXL_STATUS_OK)
    {
        std::cerr << "mxlCreateFlowWriter failed\n";
        return 1;
    }
    std::cout << "writing " << frames << " grains to " << flowId << " created=" << created << "\n";
    for (int i = 0; i < frames; ++i)
    {
        auto const index = mxlGetCurrentIndex(&info.common.grainRate);
        mxlGrainInfo grain{};
        std::uint8_t* payload = nullptr;
        if (mxlFlowWriterOpenGrain(writer, index, &grain, &payload) != MXL_STATUS_OK)
        {
            std::cerr << "open failed at " << index << "\n";
            return 1;
        }
        if (payload != nullptr && grain.grainSize >= 8)
        {
            std::memcpy(payload, &index, sizeof(index));
        }
        grain.validSlices = grain.totalSlices;
        grain.flags &= ~MXL_GRAIN_FLAG_INVALID;
        grain.index = index;
        if (mxlFlowWriterCommitGrain(writer, &grain) != MXL_STATUS_OK)
        {
            std::cerr << "commit failed\n";
            return 1;
        }
        std::cout << index << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    mxlReleaseFlowWriter(instance, writer);
    mxlDestroyInstance(instance);
    return 0;
}
