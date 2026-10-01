#pragma once

#include <optional>
#include <string>
#include <vector>

namespace mfa
{
struct TargetSlot
{
    std::string dest_host_id;
    std::string target_info;
    std::string state = "pending";
};

struct SourceReplication
{
    std::string id;
    std::string domain_id;
    std::string flow_id;
    std::string local_addr;
    std::string provider;
    std::vector<TargetSlot> targets;
};

struct PostRequest
{
    std::string domain_id;
    std::string flow_id;
    std::string dest_host_id;
    std::string target_info;
    std::string local_addr;
    std::string provider;
};

struct PostResult
{
    int status = 201;
    std::string replication_id;
    std::string state;
    bool created = false;
    bool replaced = false;
};

// Idempotent source-side handshake table. Same (flow_id, dest_host_id) returns
// the existing replication. A changed target_info replaces that target in place.
class HandshakeTable
{
public:
    PostResult post(PostRequest const& request);
    bool eraseTarget(std::string const& replicationId, std::string const& destHostId);
    std::optional<SourceReplication> find(std::string const& replicationId) const;
    std::vector<SourceReplication> list() const;
    void setTargetState(std::string const& replicationId, std::string const& destHostId, std::string const& state);

private:
    std::vector<SourceReplication> rows_;
};
} // namespace mfa
