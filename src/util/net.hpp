#pragma once

#include <set>
#include <string>
#include <vector>

namespace mfa
{
std::string localHostname();
std::set<std::string> localAddresses();
// First non-loopback IPv4, in interface order. Empty when the host has none.
std::string firstNonLoopbackIPv4();
// True for a unicast IP literal that is not loopback and not 0.0.0.0 / ::.
bool announceableAddress(std::string_view host, std::string* normalized, std::string* error);
int taiOffsetSeconds();
bool pathIsTmpfs(std::string const& path);

// Resolve a DNS name to A/AAAA addresses. Results, including failure, are cached
// for about 30 s. The lookup itself is bounded so a stuck resolver cannot stall
// the caller. An unresolvable name is logged once at debug.
std::vector<std::string> resolveNameCached(std::string const& name);
} // namespace mfa
