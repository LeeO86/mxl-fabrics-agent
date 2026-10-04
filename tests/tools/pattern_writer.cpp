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

// A 1080p50 v210 flow in its own domain with the given history, written in real
// time for `seconds`; each grain carries its index in the first 8 bytes.
int writeVideo(std::string const& domain, std::string const& domainId, std::string const& flowId, int seconds, long long historyMs)
{
    std::filesystem::create_directories(domain);
    writeFile(std::filesystem::path(domain) / "options.json",
        "{\"urn:x-mxl:option:history_duration/v1.0\":" + std::to_string(historyMs * 1000000) + "}");
    writeFile(std::filesystem::path(domain) / "domain_def.json", "{\"id\":\"" + domainId + "\"}");
    std::string const def = std::string("{\"id\":\"") + flowId +
                            "\",\"format\":\"urn:x-nmos:format:video\",\"label\":\"pattern video\",\"description\":\"integration writer\","
                            "\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"pattern:Video\"]},\"parents\":[],\"media_type\":\"video/v210\","
                            "\"grain_rate\":{\"numerator\":50,\"denominator\":1},\"frame_width\":1920,\"frame_height\":1080,"
                            "\"interlace_mode\":\"progressive\",\"colorspace\":\"BT709\",\"components\":["
                            "{\"name\":\"Y\",\"width\":1920,\"height\":1080,\"bit_depth\":10},"
                            "{\"name\":\"Cb\",\"width\":960,\"height\":1080,\"bit_depth\":10},"
                            "{\"name\":\"Cr\",\"width\":960,\"height\":1080,\"bit_depth\":10}]}";
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
    std::cout << "writing 1080p50 v210 to " << flowId << " for " << seconds << " s, history " << historyMs << " ms\n";
    auto const end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::uint64_t last = 0;
    while (std::chrono::steady_clock::now() < end)
    {
        auto const index = mxlGetCurrentIndex(&info.common.grainRate);
        if (index == last)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        last = index;
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
    }
    mxlReleaseFlowWriter(instance, writer);
    mxlDestroyInstance(instance);
    return 0;
}

// Counts the grains of a (mirrored) flow that arrive with their own index stamped,
// for `seconds` after the first one. Fails below `minRate` grains per second.
int rateFlow(std::string const& domain, std::string const& flowId, int seconds, double minRate)
{
    auto* instance = mxlCreateInstance(domain.c_str(), "");
    if (instance == nullptr)
    {
        return 1;
    }
    mxlFlowReader reader = nullptr;
    for (int i = 0; i < 100 && mxlCreateFlowReader(instance, flowId.c_str(), nullptr, &reader) != MXL_STATUS_OK; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (reader == nullptr)
    {
        std::cerr << "no flow " << flowId << " in " << domain << "\n";
        return 1;
    }
    auto stamped = [&](std::uint64_t index, std::uint64_t timeoutNs) {
        mxlGrainInfo info{};
        std::uint8_t* payload = nullptr;
        auto const status = mxlFlowReaderGetGrain(reader, index, timeoutNs, &info, &payload);
        if (status != MXL_STATUS_OK)
        {
            return status == MXL_ERR_OUT_OF_RANGE_TOO_LATE ? -1 : 0;
        }
        std::uint64_t value = 0;
        if (payload != nullptr && info.grainSize >= sizeof(value))
        {
            std::memcpy(&value, payload, sizeof(value));
        }
        return value == index && (info.flags & MXL_GRAIN_FLAG_INVALID) == 0 ? 1 : 0;
    };
    // Start at the first grain that arrived.
    std::uint64_t index = 0;
    auto const startBy = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (index == 0 && std::chrono::steady_clock::now() < startBy)
    {
        mxlFlowRuntimeInfo runtime{};
        if (mxlFlowReaderGetRuntimeInfo(reader, &runtime) == MXL_STATUS_OK && runtime.headIndex != 0 && runtime.headIndex != UINT64_MAX &&
            stamped(runtime.headIndex, 0) == 1)
        {
            index = runtime.headIndex + 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (index == 0)
    {
        std::cerr << "no grain arrived\n";
        return 1;
    }
    long long good = 0;
    long long missed = 0;
    auto const end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end)
    {
        int const result = stamped(index, 100000000);
        if (result == -1)
        {
            mxlFlowRuntimeInfo runtime{};
            mxlFlowReaderGetRuntimeInfo(reader, &runtime);
            missed += static_cast<long long>(runtime.headIndex - index);
            index = runtime.headIndex;
            continue;
        }
        good += result;
        missed += result == 1 ? 0 : 1;
        ++index;
    }
    double const rate = static_cast<double>(good) / seconds;
    std::cout << "grains " << good << " missed " << missed << " rate " << rate << "/s\n";
    mxlReleaseFlowReader(instance, reader);
    mxlDestroyInstance(instance);
    return rate >= minRate ? 0 : 2;
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "read")
    {
        return readFlow(argc > 2 ? argv[2] : "/dev/shm/mxl-b/mirror", argc > 3 ? argv[3] : "", argc > 4 ? std::atoi(argv[4]) : 20);
    }
    if (argc > 6 && std::string(argv[1]) == "video")
    {
        return writeVideo(argv[2], argv[3], argv[4], std::atoi(argv[5]), std::atoll(argv[6]));
    }
    if (argc > 5 && std::string(argv[1]) == "rate")
    {
        return rateFlow(argv[2], argv[3], std::atoi(argv[4]), std::atof(argv[5]));
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
