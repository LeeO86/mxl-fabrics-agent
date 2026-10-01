#include "util/logging.hpp"

#include <chrono>
#include <ctime>
#include <iostream>
#include <mutex>

namespace mfa::log
{
namespace
{
std::mutex gMu;
Level gLevel = Level::Info;

char const* levelName(Level level)
{
    switch (level)
    {
        case Level::Error: return "error";
        case Level::Warn: return "warn";
        case Level::Info: return "info";
        case Level::Debug: return "debug";
    }
    return "info";
}

std::string jsonEscape(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 0xf];
                }
                else
                {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string timestamp()
{
    using clock = std::chrono::system_clock;
    auto const now = clock::now();
    auto const sec = clock::to_time_t(now);
    auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
    gmtime_r(&sec, &tm);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));
    return buf;
}
} // namespace

void setLevel(std::string_view name)
{
    if (name == "error")
    {
        gLevel = Level::Error;
    }
    else if (name == "warn" || name == "warning")
    {
        gLevel = Level::Warn;
    }
    else if (name == "debug" || name == "trace")
    {
        gLevel = Level::Debug;
    }
    else
    {
        gLevel = Level::Info;
    }
}

Level level()
{
    return gLevel;
}

void write(Level level, std::string_view event, std::vector<Field> const& fields)
{
    if (static_cast<int>(level) > static_cast<int>(gLevel))
    {
        return;
    }
    std::string line = "{\"ts\":\"" + timestamp() + "\",\"level\":\"" + levelName(level) + "\",\"event\":\"" + jsonEscape(event) + "\"";
    for (auto const& field : fields)
    {
        line += ",\"";
        line += jsonEscape(field.first);
        line += "\":\"";
        line += jsonEscape(field.second);
        line += "\"";
    }
    line += "}";
    std::lock_guard const lock{gMu};
    std::cout << line << std::endl;
}
} // namespace mfa::log
