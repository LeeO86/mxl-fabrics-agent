#include "fabric/domain.hpp"

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
#include <cstring>
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

struct PickedInterface
{
    bool ok = false;
    std::string node;
    std::string service;
    std::string providerName;
    mxlFabricsProvider provider = MXL_FABRICS_PROVIDER_TCP;
    std::uint64_t flags = MXL_FABRICS_IFACE_CAP_REMOTE_WRITE;
    std::uint64_t maxMessage = 0;
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
    std::uint64_t lastCounted = MXL_UNDEFINED_INDEX; // grain index last counted in grains/bytes
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
    mxlFabricsInitiator initiator = nullptr;
    mxlFlowReader reader = nullptr;
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
    std::chrono::steady_clock::time_point progressAt{}; // last MakeProgress that returned OK
    std::chrono::steady_clock::time_point notReadySince{}; // start of the current run of NOT_READY passes
    // TRANSFER_PACING=frame: a complete grain goes out in slice batches whose starts are spread
    // over pacingSpread, instead of one burst at line rate.
    int pacingBatches = 0;
    std::chrono::nanoseconds pacingSpread{};
    std::uint16_t pacedEnd = 0;  // slices of the grain being paced, 0 = none
    std::uint16_t pacedSent = 0; // slices sent so far
    int pacedBatch = 0;          // next batch
    std::chrono::steady_clock::time_point pacedStart{};
    std::uint64_t pacedBytes = 0;
    // grain_transfer_seconds: from the grain's first transfer to the completion of its last one.
    std::chrono::steady_clock::time_point transferStart{};
    bool awaitingCompletion = false;
    std::string lastError;
    std::string state = "pending";
    std::map<std::string, mxlFabricsTargetInfo> targets;
    // The target info each target was added with, to recognise a repeated request.
    std::map<std::string, std::string> targetTexts;
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
        auto attempt = [&](std::string const& provider, bool fallback) -> bool {
            PickedInterface picked = pickInterface(fabrics_, provider, endpoint.node, endpoint.service);
            if (!picked.ok)
            {
                if (error != nullptr)
                {
                    *error = picked.error;
                }
                return false;
            }
            mxlFlowReader reader = nullptr;
            auto status = mxlCreateFlowReader(instance_, flowId.c_str(), nullptr, &reader);
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
            InitiatorSlot slot;
            slot.flowId = flowId;
            slot.initiator = initiator;
            slot.reader = reader;
            slot.info = info;
            slot.continuous = !mxlIsDiscreteDataFormat(static_cast<int>(info.common.format));
            slot.provider = provider;
            slot.fallback = fallback;
            slot.state = "pending";
            auto const rate = info.common.grainRate;
            if (!slot.continuous && endpoint.pacingBatches > 1 && rate.numerator > 0)
            {
                slot.pacingBatches = endpoint.pacingBatches;
                slot.pacingSpread = std::chrono::nanoseconds(
                    static_cast<std::int64_t>(endpoint.pacingSpread * 1e9 * static_cast<double>(rate.denominator) / static_cast<double>(rate.numerator)));
            }
            initiators_.emplace(key, std::move(slot));
            return true;
        };
        if (attempt(endpoint.provider, false))
        {
            publish();
            return true;
        }
        if (endpoint.allowTcpFallback && endpoint.provider == "verbs")
        {
            log::warn("provider_fallback", {{"from", "verbs"}, {"to", "tcp"}, {"flow_id", flowId}, {"role", "source"}});
            if (attempt("tcp", true))
            {
                publish();
                return true;
            }
        }
        return false;
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
            mxlFabricsInitiatorRemoveTarget(slot.initiator, existing->second);
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
        auto const add = mxlFabricsInitiatorAddTarget(slot.initiator, info);
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
        slot.connected = false;
        slot.state = "pending";
        restartPacedGrain(slot); // the new target needs the grain header, which only the first batch carries
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
        mxlFabricsInitiatorRemoveTarget(it->second.initiator, found->second);
        mxlFabricsFreeTargetInfo(found->second);
        it->second.targets.erase(found);
        it->second.targetTexts.erase(destHost);
        restartPacedGrain(it->second);
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

    // How long the domain thread may sleep before the next pump: until the next paced batch is
    // due, at most the usual 2 ms.
    std::chrono::steady_clock::duration waitHint() const
    {
        std::chrono::steady_clock::duration wait = std::chrono::milliseconds(2);
        auto const now = std::chrono::steady_clock::now();
        for (auto const& [_, slot] : initiators_)
        {
            if (slot.pacedEnd != 0)
            {
                auto const due = slot.pacedStart + slot.pacingSpread * slot.pacedBatch / std::min<int>(slot.pacingBatches, slot.pacedEnd);
                wait = std::min(wait, due > now ? std::chrono::steady_clock::duration(due - now) : std::chrono::steady_clock::duration::zero());
            }
        }
        return std::max<std::chrono::steady_clock::duration>(wait, std::chrono::microseconds(100));
    }

private:
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
        if (slot.initiator != nullptr && fabrics_ != nullptr)
        {
            mxlFabricsDestroyInitiator(fabrics_, slot.initiator);
            slot.initiator = nullptr;
        }
        if (slot.reader != nullptr && instance_ != nullptr)
        {
            mxlReleaseFlowReader(instance_, slot.reader);
            slot.reader = nullptr;
        }
    }

    void pumpInitiator(std::string const&, InitiatorSlot& slot)
    {
        if (slot.initiator == nullptr || slot.targets.empty())
        {
            return;
        }
        auto const progress = mxlFabricsInitiatorMakeProgressNonBlocking(slot.initiator);
        if (progress == MXL_ERR_NOT_READY || progress == MXL_ERR_INTERRUPTED)
        {
            if (slot.notReadySince == std::chrono::steady_clock::time_point{})
            {
                slot.notReadySince = std::chrono::steady_clock::now();
            }
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
        slot.inFlight = false;
        auto const now = std::chrono::steady_clock::now();
        slot.progressAt = now;
        if (slot.awaitingCompletion)
        {
            if (transferObserver_)
            {
                transferObserver_(slot.provider, std::chrono::duration<double>(now - slot.transferStart).count());
            }
            slot.awaitingCompletion = false;
        }
        // After a long wait (MXL connecting an endpoint again) a target may have missed the first
        // batch of the paced grain, which carries the grain header.
        if (slot.notReadySince != std::chrono::steady_clock::time_point{} && now - slot.notReadySince > std::chrono::milliseconds(100))
        {
            restartPacedGrain(slot);
        }
        slot.notReadySince = {};
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
        if (slot.pacedEnd != 0)
        {
            pumpPacedBatch(slot);
            return;
        }
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
        // Only complete grains are paced; partial and invalid ones go out at once as before.
        if (slot.pacingBatches > 1 && end > 1 && end == info.totalSlices)
        {
            slot.pacedEnd = end;
            slot.pacedSent = 0;
            slot.pacedBatch = 0;
            slot.pacedStart = std::chrono::steady_clock::now();
            slot.pacedBytes = info.grainSize != 0 ? info.grainSize : end;
            pumpPacedBatch(slot);
            return;
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

    // The next slice batch of the grain being paced, once its start is due. Batch k of n covers
    // slices [k·end/n, (k+1)·end/n) and may start pacingSpread·k/n after the first; the next batch
    // waits for MakeProgress to report the previous one complete.
    void pumpPacedBatch(InitiatorSlot& slot)
    {
        int const batches = std::min<int>(slot.pacingBatches, slot.pacedEnd);
        auto const now = std::chrono::steady_clock::now();
        if (now < slot.pacedStart + slot.pacingSpread * slot.pacedBatch / batches)
        {
            return;
        }
        auto const start = slot.pacedSent;
        auto const end = static_cast<std::uint16_t>(static_cast<std::uint32_t>(slot.pacedEnd) * static_cast<std::uint32_t>(slot.pacedBatch + 1) /
                                                    static_cast<std::uint32_t>(batches));
        auto const transfer = mxlFabricsInitiatorTransferGrain(slot.initiator, slot.nextIndex, start, end);
        if (transfer == MXL_ERR_NOT_READY)
        {
            return;
        }
        if (transfer != MXL_STATUS_OK)
        {
            slot.errors += 1;
            slot.lastError = "transfer " + statusText(transfer);
            slot.state = "error";
            slot.pacedEnd = 0; // the grain starts again from its first slice
            return;
        }
        if (slot.pacedBatch == 0)
        {
            slot.transferStart = now;
        }
        slot.pacedSent = end;
        slot.pacedBatch += 1;
        if (slot.pacedSent >= slot.pacedEnd)
        {
            slot.grains += 1;
            slot.bytes += slot.pacedBytes;
            slot.nextIndex += 1;
            slot.pacedEnd = 0;
            slot.awaitingCompletion = true;
        }
    }

    // Sends the grain being paced again from its first slice: a target that missed the first batch
    // has no grain header (the destination takes the grain index from it).
    static void restartPacedGrain(InitiatorSlot& slot)
    {
        slot.pacedEnd = 0;
        slot.awaitingCompletion = false;
    }

    void pumpSamples(InitiatorSlot& slot)
    {
        std::size_t maxRead = 0;
        if (mxlFlowReaderGetMaxReadLengthSamples(slot.reader, &maxRead) != MXL_STATUS_OK || maxRead == 0)
        {
            maxRead = slot.info.common.maxSyncBatchSizeHint != 0 ? slot.info.common.maxSyncBatchSizeHint : 48;
        }
        std::size_t batch = slot.info.common.maxSyncBatchSizeHint != 0 ? slot.info.common.maxSyncBatchSizeHint : maxRead;
        if (batch > maxRead)
        {
            batch = maxRead;
        }
        if (!slot.primed)
        {
            mxlFlowRuntimeInfo runtime{};
            mxlFlowReaderGetRuntimeInfo(slot.reader, &runtime);
            slot.sampleHead = runtime.headIndex;
            slot.primed = true;
        }
        mxlWrappedMultiBufferSlice payload{};
        auto const status = mxlFlowReaderGetSamplesNonBlocking(slot.reader, slot.sampleHead, batch, &payload);
        if (status == MXL_ERR_OUT_OF_RANGE_TOO_EARLY || status == MXL_ERR_TIMEOUT || status == MXL_ERR_NOT_READY)
        {
            return;
        }
        if (status == MXL_ERR_OUT_OF_RANGE_TOO_LATE)
        {
            mxlFlowRuntimeInfo runtime{};
            mxlFlowReaderGetRuntimeInfo(slot.reader, &runtime);
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
        auto const transfer = mxlFabricsInitiatorTransferSamples(slot.initiator, slot.sampleHead, batch);
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
            slot.bytes += batch * 4;
            slot.sampleHead += batch;
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
                slot.bytes += item.count * 4;
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
                // A paced grain arrives in several batches; each is committed (MXL keeps a partial
                // grain open, readers of whole grains wait for the last slice), counted once.
                if (index != slot.lastCounted)
                {
                    slot.grains += 1;
                    slot.bytes += snapshot.grainSize;
                    slot.lastCounted = index;
                }
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
                cv.wait_for(lock, session.waitHint(), [&] { return stop || !queue.empty(); });
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

void FabricDomain::keepFlowsOnExit()
{
    impl_->call([](Session& session) { session.keepFlowsOnExit(); });
}

void FabricDomain::setTransferObserver(TransferObserver observer)
{
    impl_->call([&](Session& session) { session.setTransferObserver(std::move(observer)); });
}

std::vector<FabricRow> FabricDomain::rows() const
{
    std::lock_guard const lock{impl_->rowsMu};
    return impl_->rows;
}
} // namespace mfa
