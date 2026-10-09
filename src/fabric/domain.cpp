#include "fabric/domain.hpp"

#include "replication/policy.hpp"
#include "util/jsonutil.hpp"
#include "util/logging.hpp"
#include "util/threading.hpp"

#include <algorithm>

#include <mxl/fabrics.h>
#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

namespace mfa
{
namespace
{
std::string statusText(mxlStatus status)
{
    return std::to_string(static_cast<int>(status));
}

// The fabric thread makes a pass (initiator progress, target drains) at least this often.
constexpr auto kPassInterval = std::chrono::milliseconds(2);

struct PickedInterface
{
    bool ok = false;
    std::string node;
    std::string service;
    std::string providerName;
    mxlFabricsProvider provider = MXL_FABRICS_PROVIDER_TCP;
    std::uint64_t flags = MXL_FABRICS_IFACE_CAP_REMOTE_WRITE;
    std::uint64_t maxMessage = 0;
    std::string domain; // libfabric domain (RDMA device) of that address, from the interface list
    std::string error;
};

PickedInterface pickInterface(mxlFabricsInstance fabrics, std::string const& providerName, std::string const& node, std::string const& service)
{
    PickedInterface picked;
    picked.providerName = providerName;
    mxlFabricsProvider provider = MXL_FABRICS_PROVIDER_TCP;
    if (mxlFabricsProviderFromString(providerName.c_str(), &provider) != MXL_STATUS_OK)
    {
        picked.error = "unknown provider " + providerName;
        return picked;
    }
    picked.provider = provider;
    mxlFabricsInterfaceConfig query{};
    query.version = MXL_FABRICS_API_VERSION;
    query.provider = provider;
    query.caps.version = MXL_FABRICS_API_VERSION;
    mxlFabricsInterfaceList* list = nullptr;
    auto const status = mxlFabricsGetInterfaces(fabrics, &query, &list);
    if (status != MXL_STATUS_OK || list == nullptr)
    {
        picked.error = "no " + providerName + " interface (" + statusText(status) + ")";
        return picked;
    }
    mxlFabricsInterfaceConfig const* match = nullptr;
    for (auto* it = list; it != nullptr; it = it->next)
    {
        auto const* addr = it->interface.address.node;
        if (!node.empty() && (addr == nullptr || node != addr))
        {
            continue;
        }
        match = &it->interface;
        break;
    }
    if (match == nullptr)
    {
        mxlFabricsFreeInterfaceList(list);
        picked.error = "provider " + providerName + " has no interface for " + (node.empty() ? std::string("any address") : node);
        return picked;
    }
    picked.node = match->address.node != nullptr ? match->address.node : "";
    picked.service = service.empty() ? (match->address.service != nullptr ? match->address.service : "") : service;
    picked.flags = match->caps.flags != 0 ? match->caps.flags : static_cast<std::uint64_t>(MXL_FABRICS_IFACE_CAP_REMOTE_WRITE);
    picked.maxMessage = match->caps.maxMessageSize;
    if (match->attr != nullptr)
    {
        picked.domain = json::asString(json::objectOrEmpty(json::parse(match->attr)), "fi_domain_name").value_or("");
    }
    picked.ok = true;
    mxlFabricsFreeInterfaceList(list);
    if (picked.node.empty() && !node.empty())
    {
        picked.node = node;
    }
    return picked;
}

mxlFabricsInterfaceConfig makeConfig(PickedInterface const& picked)
{
    mxlFabricsInterfaceConfig config{};
    config.version = MXL_FABRICS_API_VERSION;
    config.provider = picked.provider;
    config.caps.version = MXL_FABRICS_API_VERSION;
    config.caps.flags = picked.flags | MXL_FABRICS_IFACE_CAP_REMOTE_WRITE;
    config.caps.maxMessageSize = picked.maxMessage;
    config.address.node = picked.node.empty() ? nullptr : picked.node.c_str();
    config.address.service = picked.service.empty() ? nullptr : picked.service.c_str();
    config.attr = nullptr;
    return config;
}

std::string targetInfoToString(mxlFabricsTargetInfo info)
{
    std::size_t len = 0;
    if (mxlFabricsTargetInfoToString(info, nullptr, &len) != MXL_STATUS_OK || len == 0)
    {
        return {};
    }
    std::string text(len, '\0');
    if (mxlFabricsTargetInfoToString(info, text.data(), &len) != MXL_STATUS_OK)
    {
        return {};
    }
    if (!text.empty() && text.back() == '\0')
    {
        text.pop_back();
    }
    return text;
}

// A continuous target receives each transfer into one entry of its bounce buffer: a 16-byte header
// (MXL's AudioEntryHeader, head index and count), then the samples of all channels. MXL rejects a
// transfer that does not fit. The target info carries the entry size; 0 when it does not.
constexpr std::uint64_t kBounceEntryHeaderBytes = 16;

std::uint64_t bounceEntryBytes(std::string const& targetInfo)
{
    auto const root = json::objectOrEmpty(json::parse(targetInfo));
    auto const bounce = root.find("bounceBufferInfo");
    if (bounce == root.end() || !bounce->second.is<picojson::object>())
    {
        return 0;
    }
    return std::strtoull(json::asString(bounce->second.get<picojson::object>(), "entrySize").value_or("").c_str(), nullptr, 10);
}

struct WriterSlot
{
    mxlFlowWriter writer = nullptr;
    mxlFlowConfigInfo info{};
    bool continuous = false;
};

struct TargetSlot
{
    std::string flowId;
    std::string peer;
    mxlFabricsTarget target = nullptr;
    std::string provider;
    bool fallback = false;
    int cqDepth = 0;
    std::uint64_t grains = 0;
    std::uint64_t bytes = 0;
    std::uint64_t errors = 0;
    std::uint64_t head = 0;
    std::size_t lastBatch = 1; // samples in the last committed batch (a "grain" of a continuous flow)
    std::string lastError;
    std::string state = "pending";
    std::vector<std::uint64_t> pendingGrains;
    struct PendingSamples
    {
        std::uint64_t head = 0;
        std::size_t count = 0;
    };
    std::vector<PendingSamples> pendingSamples;
};

struct InitiatorSlot
{
    std::string flowId;
    FabricEndpoint endpoint; // where reader and initiator were opened (provider after a fallback), to open them again
    mxlFabricsInitiator initiator = nullptr;
    mxlFlowReader reader = nullptr;
    std::uint64_t inode = 0; // the origin's data file when the reader was opened
    std::chrono::steady_clock::time_point checkedAt{}; // last check of that file
    bool reopen = false;     // the reader reported the origin flow invalid (re-created)
    bool continuous = false;
    mxlFlowConfigInfo info{};
    std::string provider;
    bool fallback = false;
    std::uint64_t grains = 0;
    std::uint64_t bytes = 0;
    std::uint64_t errors = 0;
    std::uint64_t head = 0;
    std::uint64_t nextIndex = 0;
    std::uint64_t sampleHead = 0;
    bool primed = false;
    bool connected = false;
    bool inFlight = false;
    std::chrono::steady_clock::time_point completedAt{}; // pass that saw the last sample transfer complete
    std::chrono::steady_clock::time_point progressAt{};  // last MakeProgress that returned OK
    // grain_transfer_seconds: from a grain's transfer to its completion.
    std::chrono::steady_clock::time_point transferStart{};
    bool awaitingCompletion = false;
    std::string lastError;
    std::string state = "pending";
    std::map<std::string, mxlFabricsTargetInfo> targets;
    // The target info each target was added with, to recognise a repeated request.
    std::map<std::string, std::string> targetTexts;
    std::uint64_t entryBytes = 0; // smallest bounce buffer entry of the targets (continuous flows), 0 = unknown
};

class Session
{
public:
    explicit Session(std::string path)
        : path_(std::move(path))
    {}

    ~Session()
    {
        shutdown();
    }

    bool open(std::string* error)
    {
        if (instance_ != nullptr)
        {
            return true;
        }
        instance_ = mxlCreateInstance(path_.c_str(), "");
        if (instance_ == nullptr)
        {
            if (error != nullptr)
            {
                *error = "mxlCreateInstance failed for " + path_;
            }
            return false;
        }
        auto const status = mxlFabricsCreateInstance(instance_, nullptr, &fabrics_);
        if (status != MXL_STATUS_OK || fabrics_ == nullptr)
        {
            if (error != nullptr)
            {
                *error = "mxlFabricsCreateInstance failed (" + statusText(status) + ")";
            }
            mxlDestroyInstance(instance_);
            instance_ = nullptr;
            return false;
        }
        return true;
    }

    void shutdown()
    {
        for (auto& [_, slot] : initiators_)
        {
            destroyInitiatorSlot(slot);
        }
        initiators_.clear();
        for (auto& [_, slot] : targets_)
        {
            destroyTargetSlot(slot);
        }
        targets_.clear();
        // MXL deletes a flow when its last writer is released or its instance destroyed. To keep
        // the mirror flows for the next start (readers keep their flow), leave both to the exit.
        for (auto& [_, slot] : writers_)
        {
            if (slot.writer != nullptr && instance_ != nullptr && !keepFlows_)
            {
                mxlReleaseFlowWriter(instance_, slot.writer);
            }
        }
        writers_.clear();
        if (fabrics_ != nullptr)
        {
            mxlFabricsDestroyInstance(fabrics_);
            fabrics_ = nullptr;
        }
        if (instance_ != nullptr && !keepFlows_)
        {
            mxlDestroyInstance(instance_);
        }
        instance_ = nullptr;
    }

    void keepFlowsOnExit()
    {
        keepFlows_ = true;
    }

    bool ensureWriter(std::string const& flowDef, std::string* error)
    {
        if (!open(error))
        {
            return false;
        }
        std::string id;
        auto const parsed = json::parse(flowDef);
        if (parsed.is<picojson::object>())
        {
            id = json::asString(parsed.get<picojson::object>(), "id").value_or("");
        }
        if (id.empty())
        {
            if (error != nullptr)
            {
                *error = "flow definition has no id";
            }
            return false;
        }
        if (writers_.count(id) != 0)
        {
            return true;
        }
        mxlFlowConfigInfo info{};
        bool created = false;
        mxlFlowWriter writer = nullptr;
        auto const status = mxlCreateFlowWriter(instance_, flowDef.c_str(), nullptr, &writer, &info, &created);
        if (status != MXL_STATUS_OK || writer == nullptr)
        {
            if (error != nullptr)
            {
                *error = "mxlCreateFlowWriter failed (" + statusText(status) + ")";
            }
            return false;
        }
        auto& slot = writers_[id];
        if (slot.writer != nullptr && slot.writer != writer)
        {
            mxlReleaseFlowWriter(instance_, slot.writer);
        }
        slot.writer = writer;
        slot.info = info;
        slot.continuous = !mxlIsDiscreteDataFormat(static_cast<int>(info.common.format));
        return true;
    }

    bool commitPattern(std::string const& flowId, std::uint64_t* index, std::string* error)
    {
        auto const it = writers_.find(flowId);
        if (it == writers_.end() || it->second.writer == nullptr)
        {
            if (error != nullptr)
            {
                *error = "writer missing";
            }
            return false;
        }
        auto const rate = it->second.info.common.grainRate;
        auto const grainIndex = mxlGetCurrentIndex(&rate);
        mxlGrainInfo info{};
        std::uint8_t* payload = nullptr;
        auto const opened = mxlFlowWriterOpenGrain(it->second.writer, grainIndex, &info, &payload);
        if (opened != MXL_STATUS_OK)
        {
            if (error != nullptr)
            {
                *error = "open grain " + statusText(opened);
            }
            return false;
        }
        if (payload != nullptr && info.grainSize > 0)
        {
            std::memset(payload, 0xA5, info.grainSize);
        }
        info.validSlices = info.totalSlices;
        info.flags &= ~MXL_GRAIN_FLAG_INVALID;
        info.index = grainIndex;
        auto const committed = mxlFlowWriterCommitGrain(it->second.writer, &info);
        if (committed != MXL_STATUS_OK)
        {
            if (error != nullptr)
            {
                *error = "commit grain " + statusText(committed);
            }
            return false;
        }
        if (index != nullptr)
        {
            *index = grainIndex;
        }
        return true;
    }

    void releaseWriter(std::string const& flowId)
    {
        auto const it = writers_.find(flowId);
        if (it == writers_.end())
        {
            return;
        }
        if (it->second.writer != nullptr && instance_ != nullptr)
        {
            mxlReleaseFlowWriter(instance_, it->second.writer);
        }
        writers_.erase(it);
    }

    std::uint64_t headIndex(std::string const& flowId)
    {
        mxlFlowReader reader = nullptr;
        if (mxlCreateFlowReader(instance_, flowId.c_str(), nullptr, &reader) != MXL_STATUS_OK || reader == nullptr)
        {
            return 0;
        }
        mxlFlowRuntimeInfo runtime{};
        mxlFlowReaderGetRuntimeInfo(reader, &runtime);
        mxlReleaseFlowReader(instance_, reader);
        return runtime.headIndex;
    }

    TargetSetup setupTarget(std::string const& key, std::string const& flowId, FabricEndpoint const& endpoint, int cqDepth)
    {
        TargetSetup result;
        std::string error;
        if (!open(&error))
        {
            result.error = error;
            return result;
        }
        auto const writer = writers_.find(flowId);
        if (writer == writers_.end() || writer->second.writer == nullptr)
        {
            result.error = "mirror writer is not open";
            return result;
        }
        destroyTarget(key);
        auto attempt = [&](std::string const& provider, bool fallback) -> bool {
            PickedInterface picked = pickInterface(fabrics_, provider, endpoint.node, endpoint.service);
            if (!picked.ok)
            {
                result.error = picked.error;
                return false;
            }
            mxlFabricsTarget target = nullptr;
            if (mxlFabricsCreateTarget(fabrics_, &target) != MXL_STATUS_OK || target == nullptr)
            {
                result.error = "mxlFabricsCreateTarget failed";
                return false;
            }
            auto const configIface = makeConfig(picked);
            mxlFabricsTargetConfig config{};
            config.version = MXL_FABRICS_API_VERSION;
            config.interface = configIface;
            config.writer = writer->second.writer;
            auto const options = std::string("{\"cqDepth\":") + std::to_string(cqDepth) + "}";
            mxlFabricsTargetInfo info = nullptr;
            auto const status = mxlFabricsTargetSetup(target, &config, options.c_str(), &info);
            if (status != MXL_STATUS_OK || info == nullptr)
            {
                mxlFabricsDestroyTarget(fabrics_, target);
                result.error = "mxlFabricsTargetSetup failed (" + statusText(status) + ")";
                return false;
            }
            auto text = targetInfoToString(info);
            mxlFabricsFreeTargetInfo(info);
            if (text.empty())
            {
                mxlFabricsDestroyTarget(fabrics_, target);
                result.error = "target info serialization failed";
                return false;
            }
            TargetSlot slot;
            slot.flowId = flowId;
            slot.target = target;
            slot.provider = provider;
            slot.fallback = fallback;
            slot.cqDepth = cqDepth;
            slot.state = "pending";
            targets_[key] = slot;
            // The device a target or initiator uses, to compare with the device of a queue pair (rdma res).
            log::info("fabric_endpoint", {{"role", "destination"}, {"flow_id", flowId}, {"provider", provider}, {"address", picked.node},
                                             {"domain", picked.domain}});
            result.ok = true;
            result.target_info = std::move(text);
            result.provider_used = provider;
            result.fallback = fallback;
            result.error.clear();
            return true;
        };
        if (!attempt(endpoint.provider, false) && endpoint.allowTcpFallback && endpoint.provider == "verbs")
        {
            log::warn("provider_fallback", {{"from", "verbs"}, {"to", "tcp"}, {"flow_id", flowId}});
            attempt("tcp", true);
        }
        publish();
        return result;
    }

    void destroyTarget(std::string const& key)
    {
        auto const it = targets_.find(key);
        if (it == targets_.end())
        {
            return;
        }
        destroyTargetSlot(it->second);
        targets_.erase(it);
        publish();
    }

    bool ensureInitiator(std::string const& key, std::string const& flowId, FabricEndpoint const& endpoint, std::string* error)
    {
        if (!open(error))
        {
            return false;
        }
        if (initiators_.count(key) != 0)
        {
            return true;
        }
        InitiatorSlot slot;
        slot.flowId = flowId;
        slot.endpoint = endpoint;
        if (!openInitiator(slot, error))
        {
            if (!endpoint.allowTcpFallback || endpoint.provider != "verbs")
            {
                return false;
            }
            log::warn("provider_fallback", {{"from", "verbs"}, {"to", "tcp"}, {"flow_id", flowId}, {"role", "source"}});
            slot.endpoint.provider = "tcp";
            slot.fallback = true;
            if (!openInitiator(slot, error))
            {
                return false;
            }
        }
        slot.provider = slot.endpoint.provider;
        slot.checkedAt = std::chrono::steady_clock::now();
        initiators_.emplace(key, std::move(slot));
        publish();
        return true;
    }

    // Opens the slot's reader on the origin flow and an initiator for it on the slot's endpoint.
    bool openInitiator(InitiatorSlot& slot, std::string* error)
    {
        // Before the reader opens it: a flow created again in between is then seen as a new inode.
        auto const inode = fileInode(dataPath(slot.flowId));
        if (inode == 0)
        {
            if (error != nullptr)
            {
                *error = "origin flow not found";
            }
            return false;
        }
        PickedInterface picked = pickInterface(fabrics_, slot.endpoint.provider, slot.endpoint.node, slot.endpoint.service);
        if (!picked.ok)
        {
            if (error != nullptr)
            {
                *error = picked.error;
            }
            return false;
        }
        mxlFlowReader reader = nullptr;
        auto status = mxlCreateFlowReader(instance_, slot.flowId.c_str(), nullptr, &reader);
        if (status != MXL_STATUS_OK || reader == nullptr)
        {
            if (error != nullptr)
            {
                *error = "mxlCreateFlowReader failed (" + statusText(status) + ")";
            }
            return false;
        }
        mxlFlowConfigInfo info{};
        mxlFlowReaderGetConfigInfo(reader, &info);
        mxlFabricsInitiator initiator = nullptr;
        status = mxlFabricsCreateInitiator(fabrics_, &initiator);
        if (status != MXL_STATUS_OK || initiator == nullptr)
        {
            mxlReleaseFlowReader(instance_, reader);
            if (error != nullptr)
            {
                *error = "mxlFabricsCreateInitiator failed";
            }
            return false;
        }
        auto const iface = makeConfig(picked);
        mxlFabricsInitiatorConfig config{};
        config.version = MXL_FABRICS_API_VERSION;
        config.interface = iface;
        config.reader = reader;
        status = mxlFabricsInitiatorSetup(initiator, &config, nullptr);
        if (status != MXL_STATUS_OK)
        {
            mxlFabricsDestroyInitiator(fabrics_, initiator);
            mxlReleaseFlowReader(instance_, reader);
            if (error != nullptr)
            {
                *error = "mxlFabricsInitiatorSetup failed (" + statusText(status) + ")";
            }
            return false;
        }
        log::info("fabric_endpoint", {{"role", "source"}, {"flow_id", slot.flowId}, {"provider", slot.endpoint.provider}, {"address", picked.node},
                                         {"domain", picked.domain}});
        slot.initiator = initiator;
        slot.reader = reader;
        slot.inode = inode;
        slot.info = info;
        slot.continuous = !mxlIsDiscreteDataFormat(static_cast<int>(info.common.format));
        return true;
    }

    std::string dataPath(std::string const& flowId) const
    {
        return (std::filesystem::path(path_) / (flowId + ".mxl-flow") / "data").string();
    }

    bool addInitiatorTarget(std::string const& key, std::string const& destHost, std::string const& targetInfo, std::string* error)
    {
        auto const it = initiators_.find(key);
        if (it == initiators_.end())
        {
            if (error != nullptr)
            {
                *error = "initiator missing";
            }
            return false;
        }
        auto& slot = it->second;
        auto existing = slot.targets.find(destHost);
        if (existing != slot.targets.end())
        {
            // The destination repeats its request on every reconcile pass. Removing
            // and adding the same target dropped the connection each time (every
            // 0.4 s in the integration test), and a 200 ms ring lost half its grains
            // while the initiator reconnected.
            if (slot.targetTexts[destHost] == targetInfo)
            {
                return true;
            }
            if (slot.initiator != nullptr)
            {
                mxlFabricsInitiatorRemoveTarget(slot.initiator, existing->second);
            }
            mxlFabricsFreeTargetInfo(existing->second);
            slot.targets.erase(existing);
            slot.targetTexts.erase(destHost);
        }
        mxlFabricsTargetInfo info = nullptr;
        auto const status = mxlFabricsTargetInfoFromString(targetInfo.c_str(), &info);
        if (status != MXL_STATUS_OK || info == nullptr)
        {
            if (error != nullptr)
            {
                *error = "bad target info";
            }
            return false;
        }
        // Without an initiator (the origin flow is gone) the target is added when the flow is back.
        auto const add = slot.initiator != nullptr ? mxlFabricsInitiatorAddTarget(slot.initiator, info) : MXL_STATUS_OK;
        if (add != MXL_STATUS_OK)
        {
            mxlFabricsFreeTargetInfo(info);
            if (error != nullptr)
            {
                *error = "mxlFabricsInitiatorAddTarget failed (" + statusText(add) + ")";
            }
            return false;
        }
        slot.targets.emplace(destHost, info);
        slot.targetTexts[destHost] = targetInfo;
        updateEntryBytes(slot);
        slot.connected = false;
        slot.state = "pending";
        publish();
        return true;
    }

    void removeInitiatorTarget(std::string const& key, std::string const& destHost)
    {
        auto const it = initiators_.find(key);
        if (it == initiators_.end())
        {
            return;
        }
        auto found = it->second.targets.find(destHost);
        if (found == it->second.targets.end())
        {
            return;
        }
        if (it->second.initiator != nullptr)
        {
            mxlFabricsInitiatorRemoveTarget(it->second.initiator, found->second);
        }
        mxlFabricsFreeTargetInfo(found->second);
        it->second.targets.erase(found);
        it->second.targetTexts.erase(destHost);
        updateEntryBytes(it->second);
        publish();
    }

    void destroyInitiator(std::string const& key)
    {
        auto const it = initiators_.find(key);
        if (it == initiators_.end())
        {
            return;
        }
        destroyInitiatorSlot(it->second);
        initiators_.erase(it);
        publish();
    }

    void pump()
    {
        for (auto& [key, slot] : initiators_)
        {
            pumpInitiator(key, slot);
        }
        for (auto& [key, slot] : targets_)
        {
            pumpTarget(key, slot);
        }
        publish();
    }

    std::vector<FabricRow> rows() const
    {
        return rows_;
    }

    std::optional<std::uint64_t> originHead(std::string const& key) const
    {
        auto const it = initiators_.find(key);
        mxlFlowRuntimeInfo runtime{};
        if (it == initiators_.end() || it->second.reader == nullptr ||
            mxlFlowReaderGetRuntimeInfo(it->second.reader, &runtime) != MXL_STATUS_OK || runtime.headIndex == MXL_UNDEFINED_INDEX)
        {
            return std::nullopt;
        }
        return runtime.headIndex;
    }

    void setTransferObserver(TransferObserver observer)
    {
        transferObserver_ = std::move(observer);
    }

private:
    static void updateEntryBytes(InitiatorSlot& slot)
    {
        slot.entryBytes = 0;
        for (auto const& [_, text] : slot.targetTexts)
        {
            auto const bytes = bounceEntryBytes(text);
            if (bytes != 0 && (slot.entryBytes == 0 || bytes < slot.entryBytes))
            {
                slot.entryBytes = bytes;
            }
        }
    }

    void destroyTargetSlot(TargetSlot& slot)
    {
        if (slot.target != nullptr && fabrics_ != nullptr)
        {
            mxlFabricsDestroyTarget(fabrics_, slot.target);
            slot.target = nullptr;
        }
    }

    void destroyInitiatorSlot(InitiatorSlot& slot)
    {
        for (auto& [_, info] : slot.targets)
        {
            if (slot.initiator != nullptr)
            {
                mxlFabricsInitiatorRemoveTarget(slot.initiator, info);
            }
            mxlFabricsFreeTargetInfo(info);
        }
        slot.targets.clear();
        slot.targetTexts.clear();
        closeInitiator(slot);
    }

    // Closes initiator (and with it the connections) and reader; the targets stay to be added again.
    void closeInitiator(InitiatorSlot& slot)
    {
        if (slot.initiator != nullptr && fabrics_ != nullptr)
        {
            mxlFabricsDestroyInitiator(fabrics_, slot.initiator);
        }
        slot.initiator = nullptr;
        if (slot.reader != nullptr && instance_ != nullptr)
        {
            mxlReleaseFlowReader(instance_, slot.reader);
        }
        slot.reader = nullptr;
        slot.primed = false;
        slot.connected = false;
        slot.inFlight = false;
        slot.awaitingCompletion = false;
    }

    // A writer that restarts releases the origin flow (MXL deletes it with its last writer) and creates
    // it again with a new inode; the reader stays on the deleted one, whose head stands still. Once a
    // second, and at once when the reader reports the flow invalid, the data file is checked. A changed
    // or deleted one closes reader and initiator; a check later (1 s, so the destinations have seen
    // the connection close and listen again) the flow is opened again, once it exists, for the same targets.
    void checkOrigin(std::string const& key, InitiatorSlot& slot)
    {
        auto const now = std::chrono::steady_clock::now();
        if (!slot.reopen && now - slot.checkedAt < std::chrono::seconds(1))
        {
            return;
        }
        slot.checkedAt = now;
        bool const invalid = std::exchange(slot.reopen, false);
        std::string reason;
        if (slot.initiator != nullptr)
        {
            reason = originChange(dataPath(slot.flowId), slot.inode);
            if (reason.empty() && invalid)
            {
                reason = "origin flow re-created"; // the reader says so
            }
        }
        if (!reason.empty())
        {
            log::warn("origin_changed", {{"replication_id", key}, {"flow_id", slot.flowId}, {"reason", reason}});
            closeInitiator(slot);
            slot.state = "pending";
            slot.lastError = reason;
            return;
        }
        if (slot.initiator != nullptr || !openInitiator(slot, nullptr))
        {
            return;
        }
        for (auto const& [_, info] : slot.targets)
        {
            mxlFabricsInitiatorAddTarget(slot.initiator, info);
        }
        log::info("origin_reopened", {{"replication_id", key}, {"flow_id", slot.flowId}});
    }

    void pumpInitiator(std::string const& key, InitiatorSlot& slot)
    {
        if (slot.targets.empty())
        {
            return;
        }
        checkOrigin(key, slot);
        if (slot.initiator == nullptr)
        {
            return;
        }
        auto const progress = mxlFabricsInitiatorMakeProgressNonBlocking(slot.initiator);
        if (progress == MXL_ERR_INTERRUPTED)
        {
            // MXL drops a target whose connection shut down: closed by the destination, or after a
            // failed transfer (libfabric's verbs provider shuts the endpoint down on a completion
            // error). With none left every MakeProgress only logs "No more targets" (39,000 lines in
            // 15 min on the platform). The targets are forgotten here; the destination repeats its
            // request on its next pass, and its target, which listens again, gets a new connection.
            for (auto& [_, info] : slot.targets)
            {
                mxlFabricsFreeTargetInfo(info);
            }
            slot.targets.clear();
            slot.targetTexts.clear();
            updateEntryBytes(slot);
            slot.connected = false;
            slot.errors += 1;
            slot.lastError = "connection shut down (failed transfer or closed by the destination)";
            log::warn("source_connection_lost", {{"replication_id", key}, {"flow_id", slot.flowId}});
            return;
        }
        if (progress == MXL_ERR_NOT_READY)
        {
            // NOT_READY also means transfers still in flight (verbs, most passes).
            // Only an initiator that never connected to its current targets is pending.
            if (!slot.connected)
            {
                slot.state = "pending";
            }
            // MXL does not count a failed transfer as complete (a queue pair out of retries), so
            // NOT_READY stays forever and nothing is sent. The destination sets the link up again
            // when its grains stop; until then the source shows it.
            else if (slot.state != "error" && std::chrono::steady_clock::now() - slot.progressAt > std::chrono::seconds(2))
            {
                slot.errors += 1;
                slot.lastError = "no transfer completed for 2 s";
                slot.state = "error";
            }
            return;
        }
        if (progress != MXL_STATUS_OK)
        {
            slot.errors += 1;
            slot.lastError = "progress " + statusText(progress);
            slot.state = "error";
            return;
        }
        slot.connected = true;
        auto const now = std::chrono::steady_clock::now();
        if (std::exchange(slot.inFlight, false))
        {
            slot.completedAt = now;
        }
        slot.progressAt = now;
        if (slot.awaitingCompletion)
        {
            if (transferObserver_)
            {
                transferObserver_(slot.provider, std::chrono::duration<double>(now - slot.transferStart).count());
            }
            slot.awaitingCompletion = false;
        }
        auto const errorsBefore = slot.errors;
        if (slot.continuous)
        {
            pumpSamples(slot);
        }
        else
        {
            pumpGrains(slot);
        }
        mxlFlowRuntimeInfo runtime{};
        if (slot.reader != nullptr && mxlFlowReaderGetRuntimeInfo(slot.reader, &runtime) == MXL_STATUS_OK)
        {
            slot.head = runtime.headIndex;
        }
        // A pass without a new error is a working link again: an earlier error is history (the
        // errors counter keeps it), not the state.
        if (slot.errors == errorsBefore)
        {
            slot.state = "active";
            slot.lastError.clear();
        }
    }

    void pumpGrains(InitiatorSlot& slot)
    {
        if (!slot.primed)
        {
            slot.nextIndex = mxlGetCurrentIndex(&slot.info.common.grainRate);
            slot.primed = true;
        }
        mxlGrainInfo info{};
        std::uint8_t* payload = nullptr;
        auto const status = mxlFlowReaderGetGrainNonBlocking(slot.reader, slot.nextIndex, &info, &payload);
        if (status == MXL_ERR_OUT_OF_RANGE_TOO_EARLY || status == MXL_ERR_TIMEOUT || status == MXL_ERR_NOT_READY)
        {
            return;
        }
        if (status == MXL_ERR_OUT_OF_RANGE_TOO_LATE)
        {
            slot.nextIndex = mxlGetCurrentIndex(&slot.info.common.grainRate);
            return;
        }
        if (status == MXL_ERR_FLOW_INVALID)
        {
            slot.reopen = true; // a writer created the flow again (1.2.3 counted this error on every pass)
            return;
        }
        if (status != MXL_STATUS_OK)
        {
            slot.errors += 1;
            slot.lastError = "get grain " + statusText(status);
            slot.state = "error";
            return;
        }
        std::uint16_t end = 0;
        if ((info.flags & MXL_GRAIN_FLAG_INVALID) == 0)
        {
            end = info.validSlices != 0 ? info.validSlices : info.totalSlices;
        }
        auto const transfer = mxlFabricsInitiatorTransferGrain(slot.initiator, slot.nextIndex, 0, end);
        if (transfer == MXL_ERR_NOT_READY)
        {
            return;
        }
        if (transfer != MXL_STATUS_OK)
        {
            slot.errors += 1;
            slot.lastError = "transfer " + statusText(transfer);
            slot.state = "error";
            return;
        }
        slot.grains += 1;
        slot.bytes += info.grainSize != 0 ? info.grainSize : end;
        slot.nextIndex += 1;
        slot.transferStart = std::chrono::steady_clock::now();
        slot.awaitingCompletion = true;
    }

    void pumpSamples(InitiatorSlot& slot)
    {
        // Over verbs MXL's target posts the receive for the next immediate only when its agent drains
        // the last transfer, once per pass of the destination's fabric thread; a transfer that comes
        // sooner meets "receiver not ready" and waits for the sender's retry timer. So a transfer
        // waits a pass after the last one completed and carries every sample written since. 1.2.2
        // sent one sync batch (48 samples from the ST 2110 gateway) per pass: half of a 48 kHz flow
        // arrived, 8 % over verbs on the platform's E810.
        if (std::chrono::steady_clock::now() - slot.completedAt < kPassInterval)
        {
            return;
        }
        mxlFlowRuntimeInfo runtime{};
        if (mxlFlowReaderGetRuntimeInfo(slot.reader, &runtime) != MXL_STATUS_OK || runtime.headIndex == MXL_UNDEFINED_INDEX)
        {
            return;
        }
        if (!slot.primed)
        {
            slot.sampleHead = runtime.headIndex;
            slot.primed = true;
        }
        if (runtime.headIndex <= slot.sampleHead)
        {
            return;
        }
        std::size_t maxRead = 0;
        if (mxlFlowReaderGetMaxReadLengthSamples(slot.reader, &maxRead) != MXL_STATUS_OK || maxRead == 0)
        {
            maxRead = slot.info.common.maxSyncBatchSizeHint != 0 ? slot.info.common.maxSyncBatchSizeHint : 48;
        }
        auto count = static_cast<std::size_t>(std::min<std::uint64_t>(runtime.headIndex - slot.sampleHead, maxRead));
        mxlWrappedMultiBufferSlice payload{};
        auto const status = mxlFlowReaderGetSamplesNonBlocking(slot.reader, slot.sampleHead + count, count, &payload);
        if (status == MXL_ERR_OUT_OF_RANGE_TOO_LATE)
        {
            slot.sampleHead = runtime.headIndex;
            return;
        }
        if (status != MXL_STATUS_OK)
        {
            slot.errors += 1;
            slot.lastError = "get samples " + statusText(status);
            slot.state = "error";
            return;
        }
        // Bytes of one sample over all channels. Without the targets' entry size the origin's sync
        // batch is the limit (the transfer size of 1.2.2), or the reader's maximum without a hint.
        auto const sampleBytes = (payload.base.fragments[0].size + payload.base.fragments[1].size) / count * payload.count;
        std::size_t limit = slot.info.common.maxSyncBatchSizeHint != 0 ? slot.info.common.maxSyncBatchSizeHint : maxRead;
        if (slot.entryBytes > kBounceEntryHeaderBytes)
        {
            limit = static_cast<std::size_t>((slot.entryBytes - kBounceEntryHeaderBytes) / sampleBytes);
        }
        count = std::min(count, limit);
        auto const transfer = mxlFabricsInitiatorTransferSamples(slot.initiator, slot.sampleHead + count, count);
        if (transfer != MXL_STATUS_OK && transfer != MXL_ERR_NOT_READY)
        {
            slot.errors += 1;
            slot.lastError = "transfer samples " + statusText(transfer);
            slot.state = "error";
            return;
        }
        if (transfer == MXL_STATUS_OK)
        {
            slot.grains += 1;
            slot.bytes += count * sampleBytes;
            slot.sampleHead += count;
            slot.inFlight = true;
        }
    }

    void pumpTarget(std::string const&, TargetSlot& slot)
    {
        auto const writer = writers_.find(slot.flowId);
        if (writer == writers_.end() || slot.target == nullptr)
        {
            return;
        }
        auto const errorsBefore = slot.errors;
        auto const grainsBefore = slot.grains;
        if (writer->second.continuous)
        {
            for (int n = 0; n < 32; ++n)
            {
                std::uint64_t head = 0;
                std::size_t count = 0;
                auto const status = mxlFabricsTargetReadSamplesNonBlocking(slot.target, &head, &count);
                if (status == MXL_ERR_NOT_READY || status == MXL_ERR_INTERRUPTED)
                {
                    break;
                }
                if (status != MXL_STATUS_OK)
                {
                    slot.errors += 1;
                    slot.lastError = "read samples " + statusText(status);
                    slot.state = "error";
                    return;
                }
                slot.pendingSamples.push_back(TargetSlot::PendingSamples{head, count});
            }
            int committed = 0;
            while (!slot.pendingSamples.empty() && committed < 8)
            {
                auto const item = slot.pendingSamples.front();
                slot.pendingSamples.erase(slot.pendingSamples.begin());
                mxlMutableWrappedMultiBufferSlice scratch{};
                auto const opened = mxlFlowWriterOpenSamples(writer->second.writer, item.head, item.count, &scratch);
                if (opened != MXL_STATUS_OK)
                {
                    slot.errors += 1;
                    slot.lastError = "open samples " + statusText(opened);
                    slot.state = "error";
                    return;
                }
                auto const commit = mxlFlowWriterCommitSamples(writer->second.writer);
                if (commit != MXL_STATUS_OK)
                {
                    slot.errors += 1;
                    slot.lastError = "commit samples " + statusText(commit);
                    slot.state = "error";
                    return;
                }
                slot.grains += 1;
                slot.bytes += (scratch.base.fragments[0].size + scratch.base.fragments[1].size) * scratch.count;
                slot.head = item.head;
                slot.lastBatch = item.count;
                ++committed;
            }
        }
        else
        {
            for (int n = 0; n < 64; ++n)
            {
                std::uint64_t index = 0;
                auto const status = mxlFabricsTargetReadGrainNonBlocking(slot.target, &index);
                if (status == MXL_ERR_NOT_READY || status == MXL_ERR_INTERRUPTED)
                {
                    break;
                }
                if (status != MXL_STATUS_OK)
                {
                    slot.errors += 1;
                    slot.lastError = "read grain " + statusText(status);
                    slot.state = "error";
                    return;
                }
                slot.pendingGrains.push_back(index);
            }
            int committed = 0;
            while (!slot.pendingGrains.empty() && committed < 16)
            {
                auto const index = slot.pendingGrains.front();
                slot.pendingGrains.erase(slot.pendingGrains.begin());
                mxlGrainInfo snapshot{};
                mxlFlowWriterGetGrainInfo(writer->second.writer, index, &snapshot);
                mxlGrainInfo opened{};
                std::uint8_t* payload = nullptr;
                auto const openStatus = mxlFlowWriterOpenGrain(writer->second.writer, index, &opened, &payload);
                if (openStatus != MXL_STATUS_OK)
                {
                    slot.errors += 1;
                    slot.lastError = "open grain " + statusText(openStatus);
                    slot.state = "error";
                    return;
                }
                // OpenGrain on this MXL revision clears validSlices and the invalid flag.
                // The pre-open snapshot still has the values fabrics wrote.
                if (snapshot.version == 0)
                {
                    snapshot = opened;
                }
                snapshot.index = index;
                auto const commit = mxlFlowWriterCommitGrain(writer->second.writer, &snapshot);
                if (commit != MXL_STATUS_OK)
                {
                    slot.errors += 1;
                    slot.lastError = "commit grain " + statusText(commit);
                    slot.state = "error";
                    return;
                }
                slot.grains += 1;
                slot.bytes += snapshot.grainSize;
                slot.head = index;
                ++committed;
            }
        }
        // Back from error only when grains arrive again: a pass without data must not make a dead
        // link look active (the replication engine sets it up again, see ReplicationEngine::reconcile).
        if (slot.errors == errorsBefore && (slot.state != "error" || slot.grains > grainsBefore))
        {
            if (slot.state == "error")
            {
                slot.lastError.clear();
            }
            slot.state = slot.grains > 0 ? "active" : "pending";
        }
    }

    void publish()
    {
        rows_.clear();
        for (auto const& [key, slot] : targets_)
        {
            FabricRow row;
            row.key = key;
            row.role = "destination";
            row.flow_id = slot.flowId;
            row.peer = slot.peer;
            row.provider = slot.provider;
            row.state = slot.state;
            row.grains = slot.grains;
            row.bytes = slot.bytes;
            row.errors = slot.errors;
            row.head = slot.head;
            // Origin writers commit at the current TAI index, so the distance from it is the lag
            // (grainRate is the sample rate of a continuous flow).
            if (auto const writer = writers_.find(slot.flowId); writer != writers_.end() && slot.grains > 0)
            {
                auto const now = mxlGetCurrentIndex(&writer->second.info.common.grainRate);
                if (now != MXL_UNDEFINED_INDEX && now > slot.head)
                {
                    row.behind = (now - slot.head) / std::max<std::size_t>(slot.lastBatch, 1);
                }
            }
            row.last_error = slot.lastError;
            row.cq_depth = slot.cqDepth;
            row.fallback = slot.fallback;
            rows_.push_back(std::move(row));
        }
        for (auto const& [key, slot] : initiators_)
        {
            FabricRow row;
            row.key = key;
            row.role = "source";
            row.flow_id = slot.flowId;
            row.provider = slot.provider;
            row.state = slot.targets.empty() ? "idle" : slot.state;
            row.grains = slot.grains;
            row.bytes = slot.bytes;
            row.errors = slot.errors;
            row.head = slot.head;
            row.last_error = slot.lastError;
            row.fallback = slot.fallback;
            if (!slot.targets.empty())
            {
                row.peer = slot.targets.begin()->first;
            }
            rows_.push_back(std::move(row));
        }
    }

    std::string path_;
    mxlInstance instance_ = nullptr;
    mxlFabricsInstance fabrics_ = nullptr;
    std::map<std::string, WriterSlot> writers_;
    std::map<std::string, TargetSlot> targets_;
    std::map<std::string, InitiatorSlot> initiators_;
    std::vector<FabricRow> rows_;
    bool keepFlows_ = false;
    TransferObserver transferObserver_;
};

struct Task
{
    std::function<void(Session&)> fn;
    std::promise<void> done;
};
} // namespace

struct FabricDomain::Impl
{
    std::string path;
    int rt = 0;
    std::vector<int> cpus;
    Session session;
    std::mutex mu;
    std::condition_variable cv;
    std::vector<Task> queue;
    bool stop = false;
    std::thread thread;
    std::mutex rowsMu;
    std::vector<FabricRow> rows;

    explicit Impl(std::string inPath, int inRt, std::vector<int> inCpus)
        : path(std::move(inPath))
        , rt(inRt)
        , cpus(std::move(inCpus))
        , session(path)
    {}

    void loop()
    {
        applyThreadScheduling(rt, cpus);
        while (true)
        {
            std::vector<Task> batch;
            {
                std::unique_lock lock(mu);
                cv.wait_for(lock, kPassInterval, [&] { return stop || !queue.empty(); });
                if (stop && queue.empty())
                {
                    break;
                }
                batch.swap(queue);
            }
            for (auto& task : batch)
            {
                try
                {
                    task.fn(session);
                    task.done.set_value();
                }
                catch (...)
                {
                    task.done.set_exception(std::current_exception());
                }
            }
            if (!stop)
            {
                session.pump();
                auto snapshot = session.rows();
                std::lock_guard const lock{rowsMu};
                rows = std::move(snapshot);
            }
        }
        session.shutdown();
    }

    template <typename F>
    void call(F&& fn)
    {
        Task task;
        task.fn = std::forward<F>(fn);
        auto future = task.done.get_future();
        {
            std::lock_guard const lock{mu};
            queue.push_back(std::move(task));
        }
        cv.notify_one();
        future.get();
    }
};

ProviderProbe probeFabrics(std::string const& scratchDir)
{
    ProviderProbe probe;
    auto* instance = mxlCreateInstance(scratchDir.c_str(), "");
    if (instance == nullptr)
    {
        probe.error = "mxlCreateInstance failed";
        return probe;
    }
    mxlFabricsInstance fabrics = nullptr;
    auto const created = mxlFabricsCreateInstance(instance, nullptr, &fabrics);
    if (created != MXL_STATUS_OK || fabrics == nullptr)
    {
        mxlDestroyInstance(instance);
        probe.error = "mxlFabricsCreateInstance failed";
        return probe;
    }
    mxlFabricsInterfaceList* list = nullptr;
    auto const status = mxlFabricsGetInterfaces(fabrics, nullptr, &list);
    if (status != MXL_STATUS_OK)
    {
        probe.error = "mxlFabricsGetInterfaces failed";
    }
    else
    {
        probe.ok = true;
        for (auto* it = list; it != nullptr; it = it->next)
        {
            char buffer[32];
            std::size_t len = sizeof(buffer);
            if (mxlFabricsProviderToString(it->interface.provider, buffer, &len) == MXL_STATUS_OK)
            {
                std::string name(buffer);
                if (std::find(probe.providers.begin(), probe.providers.end(), name) == probe.providers.end())
                {
                    probe.providers.push_back(name);
                }
            }
        }
        mxlFabricsFreeInterfaceList(list);
    }
    mxlFabricsDestroyInstance(fabrics);
    mxlDestroyInstance(instance);
    return probe;
}

FabricDomain::FabricDomain(std::string path, int rtPriority, std::vector<int> cpus)
    : path_(std::move(path))
    , impl_(std::make_unique<Impl>(path_, rtPriority, std::move(cpus)))
{
    impl_->thread = std::thread([this] { impl_->loop(); });
    std::string error;
    bool opened = false;
    impl_->call([&](Session& session) { opened = session.open(&error); });
    ok_ = opened;
    error_ = error;
    if (!ok_)
    {
        log::error("fabric_domain_open_failed", {{"path", path_}, {"error", error_}});
    }
}

FabricDomain::~FabricDomain()
{
    if (!impl_)
    {
        return;
    }
    {
        std::lock_guard const lock{impl_->mu};
        impl_->stop = true;
    }
    impl_->cv.notify_one();
    if (impl_->thread.joinable())
    {
        impl_->thread.join();
    }
}

bool FabricDomain::ensureWriter(std::string const& flowDefJson, std::string* error)
{
    bool ok = false;
    impl_->call([&](Session& session) { ok = session.ensureWriter(flowDefJson, error); });
    return ok;
}

void FabricDomain::releaseWriter(std::string const& flowId)
{
    impl_->call([&](Session& session) { session.releaseWriter(flowId); });
}

bool FabricDomain::commitPattern(std::string const& flowId, std::uint64_t* index, std::string* error)
{
    bool ok = false;
    impl_->call([&](Session& session) { ok = session.commitPattern(flowId, index, error); });
    return ok;
}

std::uint64_t FabricDomain::headIndex(std::string const& flowId)
{
    std::uint64_t head = 0;
    impl_->call([&](Session& session) { head = session.headIndex(flowId); });
    return head;
}

TargetSetup FabricDomain::setupTarget(std::string const& key, std::string const& flowId, FabricEndpoint const& endpoint, int cqDepth)
{
    TargetSetup result;
    impl_->call([&](Session& session) { result = session.setupTarget(key, flowId, endpoint, cqDepth); });
    return result;
}

void FabricDomain::destroyTarget(std::string const& key)
{
    impl_->call([&](Session& session) { session.destroyTarget(key); });
}

bool FabricDomain::ensureInitiator(std::string const& key, std::string const& flowId, FabricEndpoint const& endpoint, std::string* error)
{
    bool ok = false;
    impl_->call([&](Session& session) { ok = session.ensureInitiator(key, flowId, endpoint, error); });
    return ok;
}

bool FabricDomain::addInitiatorTarget(std::string const& key, std::string const& destHost, std::string const& targetInfo, std::string* error)
{
    bool ok = false;
    impl_->call([&](Session& session) { ok = session.addInitiatorTarget(key, destHost, targetInfo, error); });
    return ok;
}

void FabricDomain::removeInitiatorTarget(std::string const& key, std::string const& destHost)
{
    impl_->call([&](Session& session) { session.removeInitiatorTarget(key, destHost); });
}

void FabricDomain::destroyInitiator(std::string const& key)
{
    impl_->call([&](Session& session) { session.destroyInitiator(key); });
}

std::optional<std::uint64_t> FabricDomain::originHead(std::string const& key)
{
    std::optional<std::uint64_t> head;
    impl_->call([&](Session& session) { head = session.originHead(key); });
    return head;
}

void FabricDomain::setTransferObserver(TransferObserver observer)
{
    impl_->call([&](Session& session) { session.setTransferObserver(std::move(observer)); });
}

void FabricDomain::keepFlowsOnExit()
{
    impl_->call([](Session& session) { session.keepFlowsOnExit(); });
}

std::vector<FabricRow> FabricDomain::rows() const
{
    std::lock_guard const lock{impl_->rowsMu};
    return impl_->rows;
}
} // namespace mfa
