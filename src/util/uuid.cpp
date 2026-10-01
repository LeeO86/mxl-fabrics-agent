#include "util/uuid.hpp"

#include <array>
#include <cstring>
#include <cstdio>
#include <random>
#include <vector>

namespace mfa
{
namespace
{
std::uint32_t rotl(std::uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

std::array<std::uint8_t, 20> sha1(std::uint8_t const* data, std::size_t len)
{
    std::uint32_t h0 = 0x67452301;
    std::uint32_t h1 = 0xEFCDAB89;
    std::uint32_t h2 = 0x98BADCFE;
    std::uint32_t h3 = 0x10325476;
    std::uint32_t h4 = 0xC3D2E1F0;
    std::uint64_t const bits = static_cast<std::uint64_t>(len) * 8;
    std::size_t const padded = ((len + 9 + 63) / 64) * 64;
    std::vector<std::uint8_t> buf(padded, 0);
    std::memcpy(buf.data(), data, len);
    buf[len] = 0x80;
    for (int i = 0; i < 8; ++i)
    {
        buf[padded - 1 - i] = static_cast<std::uint8_t>((bits >> (8 * i)) & 0xff);
    }
    for (std::size_t off = 0; off < padded; off += 64)
    {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
        {
            w[i] = (std::uint32_t(buf[off + i * 4]) << 24) | (std::uint32_t(buf[off + i * 4 + 1]) << 16) |
                   (std::uint32_t(buf[off + i * 4 + 2]) << 8) | std::uint32_t(buf[off + i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i)
        {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i)
        {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20)
            {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            }
            else if (i < 40)
            {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            }
            else if (i < 60)
            {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            }
            else
            {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            std::uint32_t const temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }
    std::array<std::uint8_t, 20> out{};
    std::uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i)
    {
        out[i * 4] = static_cast<std::uint8_t>((hs[i] >> 24) & 0xff);
        out[i * 4 + 1] = static_cast<std::uint8_t>((hs[i] >> 16) & 0xff);
        out[i * 4 + 2] = static_cast<std::uint8_t>((hs[i] >> 8) & 0xff);
        out[i * 4 + 3] = static_cast<std::uint8_t>(hs[i] & 0xff);
    }
    return out;
}
} // namespace

std::string Uuid::str() const
{
    char buf[37];
    std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0], bytes[1], bytes[2],
        bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return buf;
}

bool Uuid::isNil() const
{
    for (auto b : bytes)
    {
        if (b != 0)
        {
            return false;
        }
    }
    return true;
}

std::optional<Uuid> parseUuid(std::string_view text)
{
    std::string hex;
    hex.reserve(32);
    for (char c : text)
    {
        if (c == '-')
        {
            continue;
        }
        hex.push_back(c);
    }
    if (hex.size() != 32)
    {
        return std::nullopt;
    }
    Uuid id;
    for (int i = 0; i < 16; ++i)
    {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9')
            {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f')
            {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F')
            {
                return c - 'A' + 10;
            }
            return -1;
        };
        int hi = nibble(hex[i * 2]);
        int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
        {
            return std::nullopt;
        }
        id.bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return id;
}

Uuid uuidV4()
{
    std::random_device rd;
    Uuid id;
    for (auto& b : id.bytes)
    {
        b = static_cast<std::uint8_t>(rd());
    }
    id.bytes[6] = static_cast<std::uint8_t>((id.bytes[6] & 0x0f) | 0x40);
    id.bytes[8] = static_cast<std::uint8_t>((id.bytes[8] & 0x3f) | 0x80);
    return id;
}

Uuid uuidV5(Uuid const& namespaceId, std::string_view name)
{
    std::vector<std::uint8_t> data(16 + name.size());
    std::memcpy(data.data(), namespaceId.bytes, 16);
    std::memcpy(data.data() + 16, name.data(), name.size());
    auto const dig = sha1(data.data(), data.size());
    Uuid id;
    std::memcpy(id.bytes, dig.data(), 16);
    id.bytes[6] = static_cast<std::uint8_t>((id.bytes[6] & 0x0f) | 0x50);
    id.bytes[8] = static_cast<std::uint8_t>((id.bytes[8] & 0x3f) | 0x80);
    return id;
}
} // namespace mfa
