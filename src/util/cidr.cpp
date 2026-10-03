#include "util/cidr.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstring>

namespace mfa
{
namespace
{
bool prefixMatches(std::uint8_t const* address, std::uint8_t const* network, int prefix)
{
    int const full = prefix / 8;
    int const rem = prefix % 8;
    if (full > 0 && std::memcmp(address, network, static_cast<std::size_t>(full)) != 0)
    {
        return false;
    }
    if (rem == 0)
    {
        return true;
    }
    auto const mask = static_cast<std::uint8_t>(0xFFu << (8 - rem));
    return (address[full] & mask) == (network[full] & mask);
}
} // namespace

std::optional<IpCidr> parseIpCidr(std::string const& text)
{
    auto const slash = text.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= text.size())
    {
        return std::nullopt;
    }
    if (text.find('/', slash + 1) != std::string::npos)
    {
        return std::nullopt;
    }
    auto const address = text.substr(0, slash);
    auto const prefixText = text.substr(slash + 1);
    int prefix = 0;
    for (char c : prefixText)
    {
        if (c < '0' || c > '9')
        {
            return std::nullopt;
        }
        prefix = prefix * 10 + (c - '0');
        if (prefix > 128)
        {
            return std::nullopt;
        }
    }
    IpCidr cidr;
    cidr.text = text;
    cidr.prefix = prefix;
    if (inet_pton(AF_INET, address.c_str(), cidr.bytes) == 1)
    {
        if (prefix > 32)
        {
            return std::nullopt;
        }
        cidr.family = AF_INET;
        return cidr;
    }
    std::memset(cidr.bytes, 0, sizeof(cidr.bytes));
    if (inet_pton(AF_INET6, address.c_str(), cidr.bytes) == 1)
    {
        cidr.family = AF_INET6;
        return cidr;
    }
    return std::nullopt;
}

bool parseIpLiteral(std::string_view host, std::string* normalized)
{
    std::string text(host);
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']')
    {
        text = text.substr(1, text.size() - 2);
    }
    unsigned char bytes[16]{};
    char printed[INET6_ADDRSTRLEN];
    if (inet_pton(AF_INET, text.c_str(), bytes) == 1)
    {
        if (normalized != nullptr && inet_ntop(AF_INET, bytes, printed, sizeof(printed)) != nullptr)
        {
            *normalized = printed;
        }
        return true;
    }
    if (inet_pton(AF_INET6, text.c_str(), bytes) == 1)
    {
        if (normalized != nullptr && inet_ntop(AF_INET6, bytes, printed, sizeof(printed)) != nullptr)
        {
            *normalized = printed;
        }
        return true;
    }
    return false;
}

bool ipInCidr(std::string const& address, IpCidr const& cidr)
{
    unsigned char bytes[16]{};
    int family = 0;
    if (inet_pton(AF_INET, address.c_str(), bytes) == 1)
    {
        family = AF_INET;
    }
    else if (inet_pton(AF_INET6, address.c_str(), bytes) == 1)
    {
        family = AF_INET6;
    }
    else
    {
        return false;
    }
    if (family != cidr.family)
    {
        return false;
    }
    return prefixMatches(bytes, cidr.bytes, cidr.prefix);
}

bool ipInCidrs(std::string const& address, std::vector<IpCidr> const& cidrs)
{
    for (auto const& cidr : cidrs)
    {
        if (ipInCidr(address, cidr))
        {
            return true;
        }
    }
    return false;
}
} // namespace mfa
