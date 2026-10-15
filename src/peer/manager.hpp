#pragma once

#include "config/config.hpp"
#include "inventory/model.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace mfa
{
struct PeerSnapshot
{
    std::string host_id;
    std::string control_url;
    std::string provider;
    std::string local_addr;
    std::string remote_addr;
    bool up = false;
    std::string boot_id;
    std::uint64_t revision = 0;
    Inventory inventory;
    std::string error;
    std::uint32_t polls = 0; // successful polls in a row of this peer process (same boot id), 0 while down
};

Inventory parseInventoryJson(std::string const& body);

// Polls in a row after this poll: 0 while the peer is down, 1 on the first poll that reads it (again, or
// after it restarted with another boot id), one more on each further poll. `previous` is the peer's
// snapshot from the poll before (none for a new peer).
std::uint32_t nextPollCount(PeerSnapshot const* previous, PeerSnapshot const& now);

// The peer's inventory lists every flow the peer holds: the peer is up, the inventory comes from a
// finished scan (revision > 0; an agent before its first scan answers revision 0 and no domain) and the
// peer has been up for one full poll (read on two polls in a row of the same process). Until then a flow
// that the inventory does not list is not missing (1.7.1).
bool inventorySettled(PeerSnapshot const& peer);

// What a peer's inventory may decide about flows and domains it does not list (origin_missing, orphans).
enum class InventoryView
{
    Down,    // down, unknown, or up but not settled for longer than the limit (a flapping peer): as if down
    Pending, // up, inventory not settled yet: wait, keep clocks paused
    Settled, // up and settled: what it does not list, it does not hold
};

// Per peer, on every reconcile pass. A peer that is up but not settled is Pending until it has been
// unsettled for `limit` (counted from the first such pass; passes while it is down do not reset it, only a
// settled one does), then Down: a peer that fails every other poll or restarts on every poll does not hold
// decisions back for good.
class InventorySettleTracker
{
public:
    void update(std::vector<PeerSnapshot> const& peers, std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration limit);
    InventoryView view(std::string const& hostId) const;
    bool anyPending() const;
    // The inventories that decide about a mirror domain: its source peer is settled and no up peer that lists
    // the domain (`holders`) is pending.
    bool settledFor(std::string const& sourceHost, std::set<std::string> const& holders) const;

private:
    std::map<std::string, InventoryView> views_;
    std::map<std::string, std::chrono::steady_clock::time_point> unsettledSince_;
};

class PeerManager
{
public:
    using Wake = std::function<void()>;

    PeerManager(Config cfg, Wake wake);
    ~PeerManager();
    void updateConfig(Config const& cfg);
    void setDiscovered(std::vector<PeerSnapshot> const& discovered);
    std::vector<PeerSnapshot> snapshot() const;
    void start();
    void stop();

private:
    void loop();
    std::vector<PeerSnapshot> mergeUnlocked() const;

    Config cfg_;
    Wake wake_;
    mutable std::mutex mu_;
    std::vector<PeerSnapshot> discovered_;
    std::vector<PeerSnapshot> live_;
    bool stop_ = false;
    std::thread thread_;
};
} // namespace mfa
