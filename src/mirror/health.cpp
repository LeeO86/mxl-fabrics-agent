#include "mirror/health.hpp"

#include <algorithm>

namespace mfa
{
std::optional<double> indexAgeMs(std::uint64_t head, std::int64_t numerator, std::int64_t denominator, double taiNow)
{
    if (numerator <= 0 || denominator <= 0)
    {
        return std::nullopt;
    }
    auto const rate = static_cast<double>(numerator) / static_cast<double>(denominator);
    auto const behind = taiNow * rate - static_cast<double>(head);
    return std::max(0.0, behind / rate * 1000.0);
}

void MirrorHealthTracker::noteRemoved(std::string const& key, std::string const& reason)
{
    auto const it = entries_.find(key);
    if (it != entries_.end())
    {
        it->second.removed_reason = reason;
    }
}

bool MirrorHealthTracker::expected(std::string const& key) const
{
    auto const it = entries_.find(key);
    return it != entries_.end() && it->second.expected;
}

std::vector<MirrorEvent> MirrorHealthTracker::update(MirrorObservation const& obs, int staleMs,
    std::chrono::steady_clock::time_point now, std::int64_t unixMs)
{
    std::vector<MirrorEvent> events;
    auto& h = entries_[obs.domain_id + "/" + obs.flow_id];
    h.domain_id = obs.domain_id;
    h.flow_id = obs.flow_id;
    if (!obs.peer.empty())
    {
        h.peer = obs.peer;
    }
    h.seen_at = now;
    auto const previous = h.state;

    // Rebuilt: a data file other than the one seen before, or the same one back after it was gone. A reader
    // that opened the old one keeps it and sees no new grain (MXL readers do not follow a new inode).
    bool rebuilt = false;
    std::uint64_t oldInode = 0;
    if (obs.inode == 0)
    {
        h.gone = h.inode != 0;
    }
    else
    {
        if (h.inode != 0 && (obs.inode != h.inode || h.gone))
        {
            rebuilt = true;
            oldInode = h.inode;
            h.rebuilds += 1;
            h.last_rebuild_unix_ms = unixMs;
            h.last_rebuild_reason = h.removed_reason.empty() ? "replaced" : h.removed_reason;
            h.expected = false; // a new file: the wait for its first grain starts again
        }
        h.inode = obs.inode;
        h.gone = false;
        h.removed_reason.clear();
    }

    h.last_grain_age_ms.reset();
    if (obs.head)
    {
        h.last_grain_age_ms = indexAgeMs(*obs.head, obs.rate_num, obs.rate_den, obs.tai_now);
    }

    if (obs.inode == 0)
    {
        h.state = "absent";
        h.reason = h.removed_reason.empty() ? "not_on_disk" : h.removed_reason;
        h.expected = false;
        h.last_grain_age_ms.reset();
    }
    else if (!obs.expected)
    {
        h.state = "idle";
        h.reason = obs.idle_reason;
        h.expected = false;
    }
    else
    {
        if (!h.expected)
        {
            h.expected = true;
            h.expected_since = now;
        }
        auto const sinceExpected = std::chrono::duration<double, std::milli>(now - h.expected_since).count();
        // A mirror that has just been asked for may hold old grains: it gets the full wait first.
        auto const waited = h.last_grain_age_ms ? std::min(*h.last_grain_age_ms, sinceExpected) : sinceExpected;
        bool const fresh = h.last_grain_age_ms && (staleMs <= 0 || *h.last_grain_age_ms < staleMs);
        h.reason.clear();
        if (staleMs <= 0 || waited < staleMs)
        {
            h.state = fresh ? "live" : "pending";
        }
        else
        {
            // Grains are missing only when the origin has newer ones: a writer that holds the flow but
            // writes nothing (no input signal) leaves the mirror quiet, not stale. Without news from the
            // source (peer or link down) the origin is taken as still written.
            bool behind = obs.origin_live;
            if (obs.origin_head)
            {
                behind = *obs.origin_head > obs.head.value_or(0);
            }
            if (!obs.blocked.empty())
            {
                behind = true;
            }
            if (behind)
            {
                h.state = "stale";
                h.reason = !obs.blocked.empty() ? obs.blocked : obs.head ? "no_new_grains" : "no_first_grain";
            }
            else
            {
                h.state = "idle";
                h.reason = "origin_silent";
            }
        }
    }

    if (rebuilt)
    {
        events.push_back(MirrorEvent{"rebuilt", h, oldInode, 0});
    }
    if (h.state == "stale" && previous != "stale")
    {
        h.stale_since = now;
        h.stale_since_unix_ms = unixMs;
        events.push_back(MirrorEvent{"stale", h, 0, 0});
    }
    else if (h.state != "stale" && previous == "stale")
    {
        auto const staleFor = std::chrono::duration<double, std::milli>(now - h.stale_since).count();
        h.stale_since_unix_ms = 0;
        events.push_back(MirrorEvent{"recovered", h, 0, staleFor});
    }
    return events;
}

void MirrorHealthTracker::prune(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration keep)
{
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        it = now - it->second.seen_at > keep ? entries_.erase(it) : std::next(it);
    }
}
} // namespace mfa
