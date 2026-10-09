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
