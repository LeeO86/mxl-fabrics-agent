#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mfa::log
{
enum class Level
{
    Error = 0,
    Warn = 1,
    Info = 2,
    Debug = 3,
};

void setLevel(std::string_view name);
Level level();

using Field = std::pair<std::string, std::string>;

void write(Level level, std::string_view event, std::vector<Field> const& fields = {});

inline void error(std::string_view event, std::vector<Field> const& fields = {})
{
    write(Level::Error, event, fields);
}
inline void warn(std::string_view event, std::vector<Field> const& fields = {})
{
    write(Level::Warn, event, fields);
}
inline void info(std::string_view event, std::vector<Field> const& fields = {})
{
    write(Level::Info, event, fields);
}
inline void debug(std::string_view event, std::vector<Field> const& fields = {})
{
    write(Level::Debug, event, fields);
}
} // namespace mfa::log
