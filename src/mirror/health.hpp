#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mfa
{
// What the controller sees of one destination mirror flow on a reconcile pass.
struct MirrorObservation
{
    std::string domain_id;
    std::string flow_id;
    std::string peer;                         // the mirror's source host
    std::uint64_t inode = 0;                  // inode of the mirror's data file, 0 = not on disk
    std::optional<std::uint64_t> head;        // newest index committed to the mirror, none before the first
    std::int64_t rate_num = 0;                // index rate: grain rate, sample rate of a continuous flow
    std::int64_t rate_den = 1;
    double tai_now = 0;                       // CLOCK_TAI seconds
    bool expected = false;                    // a replication is wanted and the origin is being written
    std::optional<std::uint64_t> origin_head; // the origin's head index from the source's inventory
    bool origin_live = false;                 // the source's inventory says the origin is live
    std::string blocked;                      // peer_down or link_down: no news from the source
    std::string idle_reason;                  // why no grain is expected (no_demand, origin_inactive, ...)
};

struct MirrorHealth
{
    std::string domain_id;
    std::string flow_id;
    std::string peer;
    std::string state = "idle"; // live, pending, stale, idle, absent
    std::string reason;
    std::optional<double> last_grain_age_ms;
    std::uint64_t rebuilds = 0;
    std::int64_t last_rebuild_unix_ms = 0; // 0 = never
    std::string last_rebuild_reason;
    std::int64_t stale_since_unix_ms = 0;  // 0 = not stale
    std::uint64_t inode = 0;               // the data file last seen (kept while the flow is gone)

    // Bookkeeping of the tracker.
    bool expected = false;
    bool gone = false;           // the data file disappeared after it was seen
    std::string removed_reason;  // the agent removed the flow: the next file is a rebuild for this reason
    std::chrono::steady_clock::time_point expected_since{};
    std::chrono::steady_clock::time_point stale_since{};
    std::chrono::steady_clock::time_point seen_at{};
};

struct MirrorEvent
{
    std::string kind; // stale, recovered, rebuilt
    MirrorHealth health;
    std::uint64_t old_inode = 0; // rebuilt: the file readers may still hold
    double stale_ms = 0;         // recovered: how long it was stale
};

// Age of index `head` at an index rate of numerator/denominator per second, in milliseconds at TAI time
// `taiNow` (0 when the index is in the future). None for a rate that is not positive.
std::optional<double> indexAgeMs(std::uint64_t head, std::int64_t numerator, std::int64_t denominator, double taiNow);

// Per destination mirror flow (key "domain_id/flow_id"): stale while grains are expected and none came for
// MIRROR_STALE_MS although the origin has newer ones, rebuilt when its data file is a new one.
class MirrorHealthTracker
{
public:
    // The agent removed the mirror flow; when it is created again, the rebuild carries this reason.
    void noteRemoved(std::string const& key, std::string const& reason);
    // `staleMs` 0 disables "stale". `unixMs` is the wall clock for the timestamps in the API.
    std::vector<MirrorEvent> update(MirrorObservation const& obs, int staleMs, std::chrono::steady_clock::time_point now,
        std::int64_t unixMs);
    // Forgets mirrors not observed for `keep` (their rebuild count with them).
    void prune(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration keep);
    bool expected(std::string const& key) const;
    std::map<std::string, MirrorHealth> const& entries() const { return entries_; }

private:
    std::map<std::string, MirrorHealth> entries_;
};
} // namespace mfa
