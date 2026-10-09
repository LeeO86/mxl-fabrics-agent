#pragma once

#include <cstdint>
#include <optional>
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

// The fabric link to a peer: the local interface of its fabric address and that interface's state.
struct FabricLink
{
    std::string netdev; // interface that holds the address, "" when there is no address or none holds it
    bool up = true;
    std::string error;                        // why it is down
    std::string rdmaDevice;                   // RDMA device of that interface, "" without one
    std::optional<std::uint64_t> retransmits; // that device's RetransSegs on port 1 (irdma), when it has one
};

// `addr` is a local fabric address (IP literal or interface name); empty is up (nothing to check).
// Carrier, RDMA device and counter come from sysfs under `sysNet` and `sysIb`.
FabricLink fabricLink(std::string const& addr, std::string const& sysNet = "/sys/class/net",
    std::string const& sysIb = "/sys/class/infiniband");
} // namespace mfa
