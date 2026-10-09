#include "replication/policy.hpp"

#include <sys/stat.h>

#include <algorithm>

namespace mfa
{
std::chrono::milliseconds nextBackoff(std::chrono::milliseconds current)
{
    return std::clamp(current * 2, std::chrono::milliseconds(250), std::chrono::milliseconds(30000));
}

bool shouldRebuild(StallInput const& in)
{
    if (!in.destError && !in.sourceError && !in.originMoved)
    {
        return false;
    }
    auto const base = in.sourceError ? std::chrono::seconds(1) : std::chrono::seconds(5);
    return in.sinceGrain > base * (1LL << std::min<std::uint64_t>(in.stalls, 3));
}

bool originGap(std::uint64_t index, std::uint64_t head, bool readable, bool invalidEmpty, std::uint64_t slotIndex)
{
    return head != UINT64_MAX && index < head && (!readable || invalidEmpty || slotIndex != index);
}

bool rescanDue(bool linkUp, std::string const& netdev, std::chrono::steady_clock::duration sinceLast)
{
    return linkUp && !netdev.empty() && sinceLast >= std::chrono::seconds(5);
}

std::uint64_t fileInode(std::string const& path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 ? static_cast<std::uint64_t>(st.st_ino) : 0;
}

std::string originChange(std::string const& dataFile, std::uint64_t inode)
{
    auto const now = fileInode(dataFile);
    if (now == 0)
    {
        return "origin flow deleted";
    }
    return now == inode ? std::string() : std::string("origin flow re-created");
}

std::string chooseOrigin(std::vector<OriginCandidate> const& candidates, std::string const& current)
{
    auto rank = [](OriginCandidate const& c) { return c.live ? 2 : c.active ? 1 : 0; };
    OriginCandidate const* best = nullptr;
    for (auto const& c : candidates)
    {
        if (best == nullptr || rank(c) > rank(*best) ||
            (rank(c) == rank(*best) && c.host != best->host && (c.host == current || (best->host != current && c.host < best->host))))
        {
            best = &c;
        }
    }
    return best != nullptr ? best->host : std::string();
}
} // namespace mfa
