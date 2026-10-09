#include "inventory/model.hpp"

#include "util/jsonutil.hpp"

#include <cmath>
#include <sstream>

namespace mfa
{
namespace
{
constexpr char const* kHistory = "urn:x-mxl:option:history_duration/v1.0";

std::int64_t rationalNum(picojson::object const& root, char const* key, bool* present)
{
    auto const it = root.find(key);
    if (it == root.end() || !it->second.is<picojson::object>())
    {
        if (present != nullptr)
        {
            *present = false;
        }
        return 0;
    }
    if (present != nullptr)
    {
        *present = true;
    }
    auto const obj = it->second.get<picojson::object>();
    return static_cast<std::int64_t>(json::asNumber(obj, "numerator").value_or(0));
}

std::int64_t rationalDen(picojson::object const& root, char const* key)
{
    auto const it = root.find(key);
    if (it == root.end() || !it->second.is<picojson::object>())
    {
        return 1;
    }
    auto const den = json::asNumber(it->second.get<picojson::object>(), "denominator").value_or(1);
    return den == 0 ? 1 : static_cast<std::int64_t>(den);
}

std::uint64_t historyNs(std::string const& optionsJson)
{
    if (optionsJson.empty())
    {
        return 200000000ULL;
    }
    auto const root = json::parse(optionsJson);
    if (!root.is<picojson::object>())
    {
        return 200000000ULL;
    }
    return static_cast<std::uint64_t>(json::asNumber(root.get<picojson::object>(), kHistory).value_or(200000000.0));
}
} // namespace

std::string Inventory::canonicalJson() const
{
    picojson::array domains;
    for (auto const& domain : this->domains)
    {
        if (domain.kind != "local")
        {
            continue;
        }
        picojson::object d;
        d["domain_id"] = picojson::value(domain.domain_id);
        d["path"] = picojson::value(domain.path);
        std::string err;
        auto options = json::parse(domain.options_json.empty() ? "{}" : domain.options_json, &err);
        d["options"] = err.empty() ? options : picojson::value(picojson::object{});
        picojson::array flows;
        for (auto const& flow : domain.flows)
        {
            picojson::object f;
            f["flow_id"] = picojson::value(flow.flow_id);
            auto def = json::parse(flow.flow_def_json.empty() ? "{}" : flow.flow_def_json, &err);
            f["flow_def"] = err.empty() ? def : picojson::value(picojson::object{});
            f["format"] = picojson::value(flow.format);
            f["media_type"] = picojson::value(flow.media_type);
            f["active"] = picojson::value(flow.active);
            f["live"] = picojson::value(flow.live);
            picojson::object grain;
            grain["numerator"] = picojson::value(static_cast<double>(flow.grain_rate_num));
            grain["denominator"] = picojson::value(static_cast<double>(flow.grain_rate_den));
            f["grain_rate"] = picojson::value(grain);
            if (flow.format == "continuous")
            {
                picojson::object sample;
                sample["numerator"] = picojson::value(static_cast<double>(flow.sample_rate_num));
                sample["denominator"] = picojson::value(static_cast<double>(flow.sample_rate_den));
                f["sample_rate"] = picojson::value(sample);
            }
            else
            {
                f["sample_rate"] = picojson::value();
            }
            f["ring_depth"] = picojson::value(static_cast<double>(flow.ring_depth));
            f["payload_size"] = picojson::value(static_cast<double>(flow.payload_size));
            flows.emplace_back(std::move(f));
        }
        d["flows"] = picojson::value(flows);
        domains.emplace_back(std::move(d));
    }
    picojson::object root;
    root["revision"] = picojson::value(static_cast<double>(revision));
    root["host_id"] = picojson::value(host_id);
    root["domains"] = picojson::value(domains);
    return picojson::value(root).serialize();
}

bool Inventory::hasLocalDomain(std::string const& domainId) const
{
    for (auto const& domain : domains)
    {
        if (domain.kind == "local" && domain.domain_id == domainId)
        {
            return true;
        }
    }
    return false;
}

std::optional<MirrorMarker> readMirrorMarker(std::string const& domainDefJson)
{
    auto const root = json::parse(domainDefJson);
    if (!root.is<picojson::object>())
    {
        return std::nullopt;
    }
    auto const& obj = root.get<picojson::object>();
    auto const it = obj.find("x-mxl-fabrics-agent");
    if (it == obj.end() || !it->second.is<picojson::object>())
    {
        return std::nullopt;
    }
    auto const& marker = it->second.get<picojson::object>();
    MirrorMarker out;
    out.mirror = json::asBool(marker, "mirror").value_or(false);
    out.source_host_id = json::asString(marker, "source_host_id").value_or("");
    out.owner_host_id = json::asString(marker, "owner_host_id").value_or("");
    return out;
}

std::string classifyDomain(std::string const& domainDefJson, std::string const& ourHostId)
{
    auto const marker = readMirrorMarker(domainDefJson);
    if (marker && marker->mirror)
    {
        if (marker->owner_host_id != ourHostId)
        {
            return "conflict";
        }
        return "mirror";
    }
    return "local";
}

bool headIsLive(std::uint64_t headIndex, std::int64_t rateNum, std::int64_t rateDen, double taiSeconds)
{
    if (headIndex == 0 || headIndex == UINT64_MAX || rateNum <= 0 || rateDen <= 0)
    {
        return false;
    }
    auto const rate = static_cast<double>(rateNum) / static_cast<double>(rateDen);
    return std::abs(static_cast<double>(headIndex) - taiSeconds * rate) <= 5.0 * rate;
}

FlowRecord flowFromDef(std::string const& flowDefJson, std::string const& optionsJson, bool active, std::uint64_t payloadOverride,
    std::uint64_t ringOverride)
{
    FlowRecord flow;
    flow.flow_def_json = flowDefJson;
    flow.active = active;
    auto const root = json::parse(flowDefJson);
    if (!root.is<picojson::object>())
    {
        return flow;
    }
    auto const& obj = root.get<picojson::object>();
    flow.flow_id = json::asString(obj, "id").value_or("");
    flow.media_type = json::asString(obj, "media_type").value_or("");
    auto const format = json::asString(obj, "format").value_or("");
    bool audio = format.find("audio") != std::string::npos;
    flow.format = audio ? "continuous" : "discrete";
    bool hasGrain = false;
    if (audio)
    {
        flow.sample_rate_num = rationalNum(obj, "sample_rate", &hasGrain);
        flow.sample_rate_den = rationalDen(obj, "sample_rate");
        flow.grain_rate_num = flow.sample_rate_num;
        flow.grain_rate_den = flow.sample_rate_den;
    }
    else
    {
        flow.grain_rate_num = rationalNum(obj, "grain_rate", &hasGrain);
        flow.grain_rate_den = rationalDen(obj, "grain_rate");
    }
    auto const history = historyNs(optionsJson);
    if (ringOverride != 0)
    {
        flow.ring_depth = ringOverride;
    }
    else if (hasGrain && flow.grain_rate_den != 0)
    {
        flow.ring_depth = history * static_cast<std::uint64_t>(flow.grain_rate_num) / static_cast<std::uint64_t>(flow.grain_rate_den) / 1000000000ULL;
        if (flow.ring_depth == 0)
        {
            flow.ring_depth = 1;
        }
    }
    if (payloadOverride != 0)
    {
        flow.payload_size = payloadOverride;
    }
    else if (flow.media_type == "video/smpte291")
    {
        flow.payload_size = 4096;
    }
    else if (flow.media_type == "video/v210" || flow.media_type == "video/v210a")
    {
        auto const width = static_cast<std::uint64_t>(json::asNumber(obj, "frame_width").value_or(0));
        auto const height = static_cast<std::uint64_t>(json::asNumber(obj, "frame_height").value_or(0));
        if (width > 0 && height > 0)
        {
            auto const line = ((width + 47) / 48) * 128;
            flow.payload_size = line * height;
            if (flow.media_type == "video/v210a")
            {
                auto const keyLine = ((width + 2) / 3) * 4;
                flow.payload_size += keyLine * height;
            }
        }
    }
    else if (audio)
    {
        auto const channels = static_cast<std::uint64_t>(json::asNumber(obj, "channel_count").value_or(1));
        auto const depth = static_cast<std::uint64_t>(json::asNumber(obj, "bit_depth").value_or(32));
        flow.payload_size = channels * (depth / 8);
    }
    return flow;
}
} // namespace mfa
