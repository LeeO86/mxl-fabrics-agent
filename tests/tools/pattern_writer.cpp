#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

#include <algorithm>
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
// time for `seconds`; each grain carries its index in the first 8 bytes. With `recreateAfter`
// (seconds, > 0) the writer is released and created again then, as by a function that restarts:
// MXL deletes the flow with its last writer and the new writer creates it with a new inode. With
// `gapEvery` (> 0) it writes that many grains, then leaves `gapLength` indexes unwritten, and so on,
// as a writer that is late and jumps to the current grain (mxl-replay's playout, FlowXer).
int writeVideo(std::string const& domain, std::string const& domainId, std::string const& flowId, int seconds, long long historyMs,
    int recreateAfter, int gapEvery, int gapLength)
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
    auto const recreateAt = std::chrono::steady_clock::now() + std::chrono::seconds(recreateAfter);
    bool recreate = recreateAfter > 0;
    std::uint64_t last = 0;
    while (std::chrono::steady_clock::now() < end)
    {
        if (recreate && std::chrono::steady_clock::now() >= recreateAt)
        {
            recreate = false;
            mxlReleaseFlowWriter(instance, writer);
            if (mxlCreateFlowWriter(instance, def.c_str(), nullptr, &writer, &info, &created) != MXL_STATUS_OK)
            {
                std::cerr << "mxlCreateFlowWriter failed\n";
                return 1;
            }
            std::cout << "re-created " << flowId << " (created " << created << ")\n";
        }
        auto const index = mxlGetCurrentIndex(&info.common.grainRate);
        if (index == last)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        last = index;
        if (gapEvery > 0 && index % (gapEvery + gapLength) >= static_cast<std::uint64_t>(gapEvery))
        {
            continue;
        }
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

// Value of sample `index` of channel `channel` in the audio pattern: never 0 (a missing sample) and
// exact in a float.
float audioSample(std::uint64_t index, std::uint32_t channel)
{
    return static_cast<float>((index + channel * 1000) % 8000000 + 1);
}

// A 2-channel 48 kHz float flow in its own domain with the given history, written in real time in
// batches of `batch` samples with that sync and commit hint (48 = 1 ms, as the ST 2110 gateway writes).
int writeAudio(std::string const& domain, std::string const& domainId, std::string const& flowId, int seconds, long long historyMs, int batch)
{
    std::filesystem::create_directories(domain);
    writeFile(std::filesystem::path(domain) / "options.json",
        "{\"urn:x-mxl:option:history_duration/v1.0\":" + std::to_string(historyMs * 1000000) + "}");
    writeFile(std::filesystem::path(domain) / "domain_def.json", "{\"id\":\"" + domainId + "\"}");
    std::string const def = std::string("{\"id\":\"") + flowId +
                            "\",\"format\":\"urn:x-nmos:format:audio\",\"label\":\"pattern audio\",\"description\":\"integration writer\","
                            "\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"pattern:Audio\"]},\"parents\":[],\"media_type\":\"audio/float32\","
                            "\"sample_rate\":{\"numerator\":48000,\"denominator\":1},\"channel_count\":2,\"bit_depth\":32}";
    auto const options = "{\"maxCommitBatchSizeHint\":" + std::to_string(batch) + ",\"maxSyncBatchSizeHint\":" + std::to_string(batch) + "}";
    auto* instance = mxlCreateInstance(domain.c_str(), "");
    if (instance == nullptr)
    {
        std::cerr << "mxlCreateInstance failed\n";
        return 1;
    }
    mxlFlowWriter writer = nullptr;
    mxlFlowConfigInfo info{};
    bool created = false;
    if (mxlCreateFlowWriter(instance, def.c_str(), options.c_str(), &writer, &info, &created) != MXL_STATUS_OK)
    {
        std::cerr << "mxlCreateFlowWriter failed\n";
        return 1;
    }
    std::cout << "writing 2 ch 48 kHz in batches of " << batch << " to " << flowId << " for " << seconds << " s, history " << historyMs << " ms\n";
    auto const end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::uint64_t head = mxlGetCurrentIndex(&info.common.grainRate);
    while (std::chrono::steady_clock::now() < end)
    {
        if (mxlGetCurrentIndex(&info.common.grainRate) < head + batch)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }
        mxlMutableWrappedMultiBufferSlice slice{};
        if (mxlFlowWriterOpenSamples(writer, head + batch, batch, &slice) != MXL_STATUS_OK)
        {
            std::cerr << "open samples failed at " << head << "\n";
            return 1;
        }
        for (std::uint32_t channel = 0; channel < slice.count; ++channel)
        {
            auto index = head;
            for (auto const& fragment : slice.base.fragments)
            {
                auto* samples = reinterpret_cast<float*>(static_cast<std::uint8_t*>(fragment.pointer) + slice.stride * channel);
                for (std::size_t i = 0; i < fragment.size / sizeof(float); ++i)
                {
                    samples[i] = audioSample(index++, channel);
                }
            }
        }
        if (mxlFlowWriterCommitSamples(writer) != MXL_STATUS_OK)
        {
            std::cerr << "commit samples failed\n";
            return 1;
        }
        head += batch;
    }
    mxlReleaseFlowWriter(instance, writer);
    mxlDestroyInstance(instance);
    return 0;
}

// Counts the samples of a (mirrored) audio pattern flow that hold their pattern value on both
// channels, for `seconds` after the first one. Fails below `minRate` samples per second.
int sampleRateFlow(std::string const& domain, std::string const& flowId, int seconds, double minRate)
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
    std::size_t maxRead = 0;
    mxlFlowReaderGetMaxReadLengthSamples(reader, &maxRead);
    mxlFlowConfigInfo config{};
    mxlFlowReaderGetConfigInfo(reader, &config);
    auto head = [&] {
        mxlFlowRuntimeInfo runtime{};
        return mxlFlowReaderGetRuntimeInfo(reader, &runtime) == MXL_STATUS_OK && runtime.headIndex != MXL_UNDEFINED_INDEX ? runtime.headIndex
                                                                                                                        : 0;
    };
    std::uint64_t index = 0;
    auto const startBy = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (index == 0 && std::chrono::steady_clock::now() < startBy)
    {
        index = head();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (index == 0)
    {
        std::cerr << "no samples arrived\n";
        return 1;
    }
    long long good = 0;
    long long missed = 0;
    // How far the mirror head is behind the current TAI sample index (the origin writes at it).
    std::uint64_t lagSum = 0;
    std::uint64_t lagMax = 0;
    std::uint64_t passes = 0;
    auto const end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        auto const now = head();
        auto const lag = mxlGetCurrentIndex(&config.common.grainRate) - now;
        lagSum += lag;
        lagMax = std::max(lagMax, lag);
        ++passes;
        if (now <= index)
        {
            continue;
        }
        if (now - index > maxRead)
        {
            missed += static_cast<long long>(now - index - maxRead);
            index = now - maxRead;
        }
        auto const count = static_cast<std::size_t>(now - index);
        mxlWrappedMultiBufferSlice slice{};
        if (mxlFlowReaderGetSamplesNonBlocking(reader, now, count, &slice) != MXL_STATUS_OK || slice.count < 2)
        {
            missed += static_cast<long long>(count);
            index = now;
            continue;
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            bool ok = true;
            for (std::uint32_t channel = 0; channel < 2; ++channel)
            {
                auto const offset = i * sizeof(float);
                auto const& first = slice.base.fragments[0];
                auto const* base = static_cast<std::uint8_t const*>(offset < first.size ? first.pointer : slice.base.fragments[1].pointer);
                float value = 0;
                std::memcpy(&value, base + slice.stride * channel + (offset < first.size ? offset : offset - first.size), sizeof(value));
                ok = ok && value == audioSample(index + i, channel);
            }
            good += ok ? 1 : 0;
            missed += ok ? 0 : 1;
        }
        index = now;
    }
    double const rate = static_cast<double>(good) / seconds;
    std::cout << "samples " << good << " missed " << missed << " rate " << rate << "/s lag avg " << lagSum / std::max<std::uint64_t>(passes, 1)
              << " max " << lagMax << " samples\n";
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
        return writeVideo(argv[2], argv[3], argv[4], std::atoi(argv[5]), std::atoll(argv[6]), argc > 7 ? std::atoi(argv[7]) : 0,
            argc > 8 ? std::atoi(argv[8]) : 0, argc > 9 ? std::atoi(argv[9]) : 3);
    }
    if (argc > 5 && std::string(argv[1]) == "rate")
    {
        return rateFlow(argv[2], argv[3], std::atoi(argv[4]), std::atof(argv[5]));
    }
    if (argc > 6 && std::string(argv[1]) == "audio")
    {
        return writeAudio(argv[2], argv[3], argv[4], std::atoi(argv[5]), std::atoll(argv[6]), argc > 7 ? std::atoi(argv[7]) : 48);
    }
    if (argc > 5 && std::string(argv[1]) == "samples")
    {
        return sampleRateFlow(argv[2], argv[3], std::atoi(argv[4]), std::atof(argv[5]));
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
