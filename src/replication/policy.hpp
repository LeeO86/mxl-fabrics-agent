#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace mfa
{
// Wait before the next try of a failed target setup or handshake: doubles from 250 ms up to 30 s.
std::chrono::milliseconds nextBackoff(std::chrono::milliseconds current);

// A destination link without a new grain, as the destination sees it on a reconcile pass.
struct StallInput
{
    std::chrono::steady_clock::duration sinceGrain{}; // since the last new grain or the target setup
    std::uint64_t stalls = 0;                         // rebuilds in a row without a new grain
    bool destError = false;                           // the destination's target reports errors
    bool sourceError = false;                         // the source answered "error" (its transfers do not complete)
    bool originMoved = false;                         // the origin was written more than 1 s after the last grain
};

// True when the destination should set its target up again: something says grains are due (an
// error on either side, or an origin that moves on) and none came for 5 s, or 1 s when the source
// reports the error. The wait doubles with each rebuild in a row, three times at most.
bool shouldRebuild(StallInput const& in);

// True when `index`, the grain a source wants to send next, is one its origin never wrote while a later
// grain exists (`head`, UINT64_MAX while none is written). A writer that skips indexes (one that is
// late and jumps to the current grain) leaves them: MXL marks them invalid with no valid slice when the
// writer opens the next grain (`invalidEmpty`); a slot that still holds another index (`slotIndex`, a
// writer that opened the flow again) or that is not complete below the head (`!readable`) was not
// written either. The head itself is the writer's latest grain and is sent, also when it is invalid.
bool originGap(std::uint64_t index, std::uint64_t head, bool readable, bool invalidEmpty, std::uint64_t slotIndex);

// True when a provider that has no interface for a local fabric address should list its interfaces
// again: a local interface holds the address and has carrier (`linkUp`, `netdev` from fabricLink), and the
// last new list is at least 5 s old. libfabric's verbs provider keeps the list of its first fi_getinfo.
bool rescanDue(bool linkUp, std::string const& netdev, std::chrono::steady_clock::duration sinceLast);

// Inode of a file, 0 when it does not exist.
std::uint64_t fileInode(std::string const& path);

// Empty while `dataFile` is still the file a reader opened (`inode`), else why not: the writer
// released the flow (MXL deletes it with its last writer) or a writer created it again.
std::string originChange(std::string const& dataFile, std::uint64_t inode);

struct OriginCandidate
{
    std::string host;
    bool active = false; // a writer holds the flow
    bool live = false;   // its head is at the current time (it is being written)
};

// The host to replicate a flow from when several hosts hold it (a function moved to another host and
// left its domain behind): a live origin before one with a writer before the rest. Among equals the
// current choice stays, else the lowest host id.
std::string chooseOrigin(std::vector<OriginCandidate> const& candidates, std::string const& current);
} // namespace mfa
