#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <picojson/picojson.h>

namespace mfa::json
{
inline picojson::value parse(std::string_view text, std::string* error = nullptr)
{
    picojson::value root;
    std::string copy(text);
    auto const err = picojson::parse(root, copy);
    if (!err.empty())
    {
        if (error != nullptr)
        {
            *error = err;
        }
        return picojson::value();
    }
    return root;
}

inline std::string stringify(picojson::value const& value)
{
    return value.serialize();
}

inline std::optional<std::string> asString(picojson::object const& obj, char const* key)
{
    auto const it = obj.find(key);
    if (it == obj.end() || !it->second.is<std::string>())
    {
        return std::nullopt;
    }
    return it->second.get<std::string>();
}

inline std::optional<bool> asBool(picojson::object const& obj, char const* key)
{
    auto const it = obj.find(key);
    if (it == obj.end() || !it->second.is<bool>())
    {
        return std::nullopt;
    }
    return it->second.get<bool>();
}

inline std::optional<double> asNumber(picojson::object const& obj, char const* key)
{
    auto const it = obj.find(key);
    if (it == obj.end() || !it->second.is<double>())
    {
        return std::nullopt;
    }
    return it->second.get<double>();
}

inline picojson::object objectOrEmpty(picojson::value const& value)
{
    if (!value.is<picojson::object>())
    {
        return {};
    }
    return value.get<picojson::object>();
}

inline std::string stringField(picojson::value const& value, char const* key, std::string const& fallback = {})
{
    if (!value.is<picojson::object>())
    {
        return fallback;
    }
    if (auto s = asString(value.get<picojson::object>(), key))
    {
        return *s;
    }
    return fallback;
}
} // namespace mfa::json
