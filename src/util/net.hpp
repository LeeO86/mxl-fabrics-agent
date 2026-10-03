#pragma once

#include <set>
#include <string>
#include <vector>

namespace mfa
{
std::string localHostname();
std::set<std::string> localAddresses();
int taiOffsetSeconds();
bool pathIsTmpfs(std::string const& path);

// Resolve a DNS name to A/AAAA addresses. Results, including failure, are cached
// for about 30 s. The lookup itself is bounded so a stuck resolver cannot stall
// the caller. An unresolvable name is logged once at debug.
std::vector<std::string> resolveNameCached(std::string const& name);
} // namespace mfa
