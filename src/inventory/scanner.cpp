#include "inventory/scanner.hpp"

#include "util/jsonutil.hpp"
#include "util/logging.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace mfa
{
namespace
{
namespace fs = std::filesystem;

std::string readFile(fs::path const& path)
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

bool flowActive(fs::path const& dataFile)
{
    int fd = ::open(dataFile.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return false;
    }
    bool const active = ::flock(fd, LOCK_EX | LOCK_NB) < 0;
    ::close(fd);
    return active;
}

struct DataHeader
{
    std::uint32_t grainCount = 0;
    std::uint64_t headIndex = 0;
};

DataHeader readDataHeader(fs::path const& dataFile)
{
    DataHeader header;
    int fd = ::open(dataFile.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return header;
    }
    // mxlFlowInfo is 2048 bytes. grainSize lives in each grain, not the flow header.
    // At stable offsets for version 1: ring depth is config.discrete.grainCount (8 byte prefix + 128 byte
    // common + 16 byte sliceSizes), the head index runtime.headIndex (8 + 192 byte config).
    unsigned char buf[208];
    auto const n = ::pread(fd, buf, sizeof(buf), 0);
    ::close(fd);
    if (n < static_cast<ssize_t>(sizeof(buf)))
    {
        return header;
    }
    std::uint32_t version = 0;
    std::memcpy(&version, buf, 4);
    if (version != 1)
    {
        return header;
    }
    std::memcpy(&header.grainCount, buf + 8 + 128 + 16, 4);
    std::memcpy(&header.headIndex, buf + 8 + 192, 8);
    return header;
}

double taiNowSeconds()
{
    timespec ts{};
    ::clock_gettime(CLOCK_TAI, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}
} // namespace

Inventory scanOnce(std::string const& root, std::string const& hostId)
{
    Inventory inventory;
    inventory.host_id = hostId;
    std::error_code ec;
    if (!fs::is_directory(root, ec))
    {
        return inventory;
    }
    std::vector<std::pair<std::string, std::string>> found;
    for (auto const& entry : fs::directory_iterator(root, ec))
    {
        if (!entry.is_directory(ec))
        {
            continue;
        }
        auto const defPath = entry.path() / "domain_def.json";
        if (!fs::is_regular_file(defPath, ec))
        {
            continue;
        }
        found.emplace_back(entry.path().string(), readFile(defPath));
    }
    auto const tai = taiNowSeconds();
    for (auto const& [path, def] : found)
    {
        auto const rootJson = json::parse(def);
        if (!rootJson.is<picojson::object>())
        {
            continue;
        }
        auto const id = json::asString(rootJson.get<picojson::object>(), "id").value_or("");
        if (id.empty())
        {
            continue;
        }
        DomainRecord domain;
        domain.domain_id = id;
        domain.path = path;
        domain.options_json = readFile(fs::path(path) / "options.json");
        domain.kind = classifyDomain(def, hostId);
        if (auto marker = readMirrorMarker(def))
        {
            domain.mirror = marker->mirror;
            domain.source_host_id = marker->source_host_id;
            domain.owner_host_id = marker->owner_host_id;
            if (marker->mirror && marker->owner_host_id != hostId)
            {
                domain.kind = "conflict";
            }
        }
        for (auto const& flowEntry : fs::directory_iterator(path, ec))
        {
            if (!flowEntry.is_directory(ec))
            {
                continue;
            }
            auto const name = flowEntry.path().filename().string();
            if (name.size() < 10 || name.substr(name.size() - 9) != ".mxl-flow")
            {
                continue;
            }
            auto const defJson = readFile(flowEntry.path() / "flow_def.json");
            if (defJson.empty())
            {
                continue;
            }
            bool const active = flowActive(flowEntry.path() / "data");
            auto flow = flowFromDef(defJson, domain.options_json, active);
            auto const header = readDataHeader(flowEntry.path() / "data");
            if (header.grainCount != 0)
            {
                flow.ring_depth = header.grainCount;
            }
            flow.live = active && headIsLive(header.headIndex, flow.grain_rate_num, flow.grain_rate_den, tai);
            domain.flows.push_back(std::move(flow));
        }
        inventory.domains.push_back(std::move(domain));
    }
    return inventory;
}

DomainScanner::DomainScanner(std::string root, std::string hostId, int intervalMs, Wake wake)
    : root_(std::move(root))
    , host_(std::move(hostId))
    , intervalMs_(intervalMs)
    , wake_(std::move(wake))
{}

DomainScanner::~DomainScanner()
{
    stop();
}

Inventory DomainScanner::snapshot() const
{
    std::lock_guard const lock{mu_};
    return inventory_;
}

void DomainScanner::start()
{
    stop_ = false;
    running_ = true;
    thread_ = std::thread([this] { loop(); });
}

void DomainScanner::stop()
{
    stop_ = true;
    if (thread_.joinable())
    {
        thread_.join();
    }
    running_ = false;
}

void DomainScanner::loop()
{
    int fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd >= 0)
    {
        ::inotify_add_watch(fd, root_.c_str(), IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM | IN_CLOSE_WRITE | IN_ATTRIB);
    }
    while (!stop_.load())
    {
        auto next = scanOnce(root_, host_);
        std::string canonical = next.canonicalJson();
        bool changed = false;
        {
            std::lock_guard const lock{mu_};
            if (canonical != lastCanonical_)
            {
                next.revision = inventory_.revision + 1;
                if (next.revision == 0)
                {
                    next.revision = 1;
                }
                lastCanonical_ = canonical;
                changed = true;
            }
            else
            {
                next.revision = inventory_.revision == 0 ? 1 : inventory_.revision;
            }
            inventory_ = std::move(next);
        }
        if (changed && wake_)
        {
            log::debug("inventory_changed", {{"revision", std::to_string(snapshot().revision)}});
            wake_();
        }
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(intervalMs_);
        while (!stop_.load() && std::chrono::steady_clock::now() < deadline)
        {
            if (fd >= 0)
            {
                char buf[4096];
                auto const n = ::read(fd, buf, sizeof(buf));
                if (n > 0)
                {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    if (fd >= 0)
    {
        ::close(fd);
    }
}
} // namespace mfa
