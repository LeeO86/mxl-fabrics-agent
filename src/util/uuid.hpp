#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mfa
{
struct Uuid
{
    std::uint8_t bytes[16]{};

    std::string str() const;
    bool isNil() const;
};

std::optional<Uuid> parseUuid(std::string_view text);
Uuid uuidV4();
Uuid uuidV5(Uuid const& namespaceId, std::string_view name);

inline Uuid nodeIdForHost(std::string_view hostId)
{
    // UUID namespace for this agent's stable Node id.
    static constexpr Uuid ns = {{0x6d, 0x78, 0x6c, 0x2d, 0x66, 0x61, 0x62, 0x72, 0x69, 0x63, 0x73, 0x2d, 0x61, 0x67, 0x65, 0x6e}};
    return uuidV5(ns, hostId);
}
} // namespace mfa
