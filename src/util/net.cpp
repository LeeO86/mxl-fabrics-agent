#include "util/net.hpp"

#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/statfs.h>
#include <sys/timex.h>
#include <unistd.h>

#include <cstring>

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
