#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mfa
{
struct IpCidr
{
    int family = 0;
    int prefix = 0;
    std::uint8_t bytes[16]{};
    std::string text;
};

std::optional<IpCidr> parseIpCidr(std::string const& text);
bool ipInCidr(std::string const& address, IpCidr const& cidr);
bool ipInCidrs(std::string const& address, std::vector<IpCidr> const& cidrs);

// True when host is an IPv4 or IPv6 literal. Bracketed IPv6 (`[::1]`) is accepted.
bool parseIpLiteral(std::string_view host, std::string* normalized = nullptr);
} // namespace mfa
