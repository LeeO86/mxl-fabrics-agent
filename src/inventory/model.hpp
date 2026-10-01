#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mfa
{
struct FlowRecord
{
    std::string flow_id;
    std::string flow_def_json;
    std::string format; // discrete or continuous
    std::string media_type;
    bool active = false;
    std::int64_t grain_rate_num = 0;
    std::int64_t grain_rate_den = 1;
    std::int64_t sample_rate_num = 0;
    std::int64_t sample_rate_den = 1;
    std::uint64_t ring_depth = 0;
    std::uint64_t payload_size = 0;
};

struct DomainRecord
{
    std::string domain_id;
    std::string path;
    std::string options_json;
    std::string kind; // local, mirror, conflict
    std::string source_host_id;
    std::string owner_host_id;
    bool mirror = false;
    std::vector<FlowRecord> flows;
};

struct Inventory
{
    std::uint64_t revision = 0;
    std::string host_id;
    std::vector<DomainRecord> domains;

    std::string canonicalJson() const;
    bool hasLocalDomain(std::string const& domainId) const;
};

struct MirrorMarker
{
    bool mirror = false;
    std::string source_host_id;
    std::string owner_host_id;
};

std::optional<MirrorMarker> readMirrorMarker(std::string const& domainDefJson);
std::string classifyDomain(std::string const& domainDefJson, std::string const& ourHostId, bool idHeldByPeer);
FlowRecord flowFromDef(std::string const& flowDefJson, std::string const& optionsJson, bool active, std::uint64_t payloadOverride = 0,
    std::uint64_t ringOverride = 0);
} // namespace mfa
