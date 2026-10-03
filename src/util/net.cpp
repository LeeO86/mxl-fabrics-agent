#include "util/net.hpp"

#include "util/cidr.hpp"
#include "util/logging.hpp"

#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/statfs.h>
#include <sys/timex.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace mfa
{
std::string localHostname()
{
    char buf[256];
    if (::gethostname(buf, sizeof(buf)) != 0)
    {
        return {};
    }
    buf[sizeof(buf) - 1] = 0;
    return buf;
}

std::set<std::string> localAddresses()
{
    std::set<std::string> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0)
    {
        return out;
    }
    for (auto* it = list; it != nullptr; it = it->ifa_next)
    {
        if (it->ifa_addr == nullptr)
        {
            continue;
        }
        char text[INET6_ADDRSTRLEN];
        if (it->ifa_addr->sa_family == AF_INET)
        {
            auto* addr = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
            if (inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) != nullptr)
            {
                out.insert(text);
            }
        }
        else if (it->ifa_addr->sa_family == AF_INET6)
        {
            auto* addr = reinterpret_cast<sockaddr_in6*>(it->ifa_addr);
            if (inet_ntop(AF_INET6, &addr->sin6_addr, text, sizeof(text)) != nullptr)
            {
                out.insert(text);
            }
        }
    }
    freeifaddrs(list);
    return out;
}

std::string firstNonLoopbackIPv4()
{
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0)
    {
        return {};
    }
    std::string found;
    for (auto* it = list; it != nullptr && found.empty(); it = it->ifa_next)
    {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }
        auto* addr = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
        char text[INET6_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) == nullptr)
        {
            continue;
        }
        std::string ip(text);
        if (ip == "0.0.0.0" || ip.rfind("127.", 0) == 0)
        {
            continue;
        }
        found = std::move(ip);
    }
    freeifaddrs(list);
    return found;
}

bool announceableAddress(std::string_view host, std::string* normalized, std::string* error)
{
    std::string norm;
    if (!parseIpLiteral(host, &norm))
    {
        if (error != nullptr)
        {
            *error = "must be an IP address literal";
        }
        return false;
    }
    bool bad = norm == "0.0.0.0" || norm == "::" || norm == "::1" || norm.rfind("127.", 0) == 0;
    if (bad)
    {
        if (error != nullptr)
        {
            *error = "must not be loopback or unspecified";
        }
        return false;
    }
    if (normalized != nullptr)
    {
        *normalized = norm;
    }
    return true;
}

int taiOffsetSeconds()
{
    timex tx{};
    tx.modes = 0;
    if (adjtimex(&tx) < 0)
    {
        return 0;
    }
    return tx.tai;
}

namespace
{
constexpr auto kResolveTimeout = std::chrono::milliseconds(250);
constexpr auto kResolveTtl = std::chrono::seconds(30);

std::string lowerName(std::string text)
{
    for (auto& c : text)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

std::vector<std::string> lookupBlocking(std::string const& name)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &result) != 0 || result == nullptr)
    {
        return {};
    }
    std::vector<std::string> addresses;
    for (auto* it = result; it != nullptr; it = it->ai_next)
    {
        char text[INET6_ADDRSTRLEN];
        if (it->ai_family == AF_INET && it->ai_addr != nullptr)
        {
            auto* addr = reinterpret_cast<sockaddr_in*>(it->ai_addr);
            if (inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) != nullptr)
            {
                addresses.emplace_back(text);
            }
        }
        else if (it->ai_family == AF_INET6 && it->ai_addr != nullptr)
        {
            auto* addr = reinterpret_cast<sockaddr_in6*>(it->ai_addr);
            if (inet_ntop(AF_INET6, &addr->sin6_addr, text, sizeof(text)) != nullptr)
            {
                addresses.emplace_back(text);
            }
        }
    }
    freeaddrinfo(result);
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    return addresses;
}

std::vector<std::string> lookupBounded(std::string const& name)
{
    struct Shared
    {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        std::vector<std::string> addresses;
    };
    auto shared = std::make_shared<Shared>();
    std::thread([shared, name] {
        auto addresses = lookupBlocking(name);
        std::lock_guard const lock{shared->mu};
        shared->addresses = std::move(addresses);
        shared->done = true;
        shared->cv.notify_one();
    }).detach();
    std::unique_lock lock{shared->mu};
    if (!shared->cv.wait_for(lock, kResolveTimeout, [&] { return shared->done; }))
    {
        return {};
    }
    return shared->addresses;
}
} // namespace

std::vector<std::string> resolveNameCached(std::string const& name)
{
    struct Entry
    {
        std::vector<std::string> addresses;
        std::chrono::steady_clock::time_point expires{};
    };
    static std::mutex mu;
    static std::map<std::string, Entry> cache;
    static std::set<std::string> loggedFailure;
    auto const key = lowerName(name);
    auto const now = std::chrono::steady_clock::now();
    {
        std::lock_guard const lock{mu};
        auto const it = cache.find(key);
        if (it != cache.end() && now < it->second.expires)
        {
            return it->second.addresses;
        }
    }
    auto addresses = lookupBounded(name);
    std::lock_guard const lock{mu};
    cache[key] = Entry{addresses, now + kResolveTtl};
    if (addresses.empty())
    {
        if (loggedFailure.insert(key).second)
        {
            log::debug("local_node_name_unresolved", {{"name", name}});
        }
    }
    else
    {
        loggedFailure.erase(key);
    }
    return addresses;
}

bool pathIsTmpfs(std::string const& path)
{
    struct statfs st{};
    if (statfs(path.c_str(), &st) != 0)
    {
        return false;
    }
    return st.f_type == 0x01021994;
}
} // namespace mfa
