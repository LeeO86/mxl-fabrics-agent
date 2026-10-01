#include "replication/handshake.hpp"

#include "util/uuid.hpp"

namespace mfa
{
PostResult HandshakeTable::post(PostRequest const& request)
{
    PostResult result;
    for (auto& row : rows_)
    {
        for (auto& target : row.targets)
        {
            if (row.flow_id == request.flow_id && target.dest_host_id == request.dest_host_id)
            {
                result.replication_id = row.id;
                result.created = false;
                if (target.target_info != request.target_info || row.domain_id != request.domain_id || row.local_addr != request.local_addr)
                {
                    target.target_info = request.target_info;
                    target.state = "pending";
                    row.domain_id = request.domain_id;
                    row.local_addr = request.local_addr;
                    row.provider = request.provider;
                    result.replaced = true;
                }
                result.state = target.state;
                return result;
            }
        }
    }
    for (auto& row : rows_)
    {
        if (row.flow_id == request.flow_id && row.domain_id == request.domain_id && row.local_addr == request.local_addr &&
            row.provider == request.provider)
        {
            TargetSlot slot;
            slot.dest_host_id = request.dest_host_id;
            slot.target_info = request.target_info;
            slot.state = "pending";
            row.targets.push_back(slot);
            result.replication_id = row.id;
            result.state = "pending";
            result.created = true;
            return result;
        }
    }
    SourceReplication row;
    row.id = uuidV4().str();
    row.domain_id = request.domain_id;
    row.flow_id = request.flow_id;
    row.local_addr = request.local_addr;
    row.provider = request.provider;
    TargetSlot slot;
    slot.dest_host_id = request.dest_host_id;
    slot.target_info = request.target_info;
    row.targets.push_back(slot);
    result.replication_id = row.id;
    result.state = "pending";
    result.created = true;
    rows_.push_back(std::move(row));
    return result;
}

bool HandshakeTable::eraseTarget(std::string const& replicationId, std::string const& destHostId)
{
    for (auto it = rows_.begin(); it != rows_.end(); ++it)
    {
        if (it->id != replicationId)
        {
            continue;
        }
        auto& targets = it->targets;
        for (auto t = targets.begin(); t != targets.end(); ++t)
        {
            if (t->dest_host_id == destHostId)
            {
                targets.erase(t);
                if (targets.empty())
                {
                    rows_.erase(it);
                }
                return true;
            }
        }
        return false;
    }
    return false;
}

std::optional<SourceReplication> HandshakeTable::find(std::string const& replicationId) const
{
    for (auto const& row : rows_)
    {
        if (row.id == replicationId)
        {
            return row;
        }
    }
    return std::nullopt;
}

std::vector<SourceReplication> HandshakeTable::list() const
{
    return rows_;
}

void HandshakeTable::setTargetState(std::string const& replicationId, std::string const& destHostId, std::string const& state)
{
    for (auto& row : rows_)
    {
        if (row.id != replicationId)
        {
            continue;
        }
        for (auto& target : row.targets)
        {
            if (target.dest_host_id == destHostId)
            {
                target.state = state;
            }
        }
    }
}
} // namespace mfa
