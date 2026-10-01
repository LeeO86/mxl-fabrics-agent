#include "util/httpclient.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <sstream>

namespace mfa
{
namespace
{
struct Url
{
    std::string host;
    std::string port = "80";
    std::string path = "/";
};

Url parseUrl(std::string const& url)
{
    Url out;
    auto rest = url;
    auto const scheme = rest.find("://");
    if (scheme != std::string::npos)
    {
        rest = rest.substr(scheme + 3);
    }
    auto const slash = rest.find('/');
    auto hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    auto const colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(':') == colon)
    {
        out.host = hostport.substr(0, colon);
        out.port = hostport.substr(colon + 1);
    }
    else
    {
        out.host = hostport;
    }
    return out;
}

bool setTimeout(int fd, std::chrono::milliseconds timeout)
{
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 && setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}
} // namespace

ClientResponse httpRequest(std::string const& method, std::string const& url, std::string const& body, std::chrono::milliseconds timeout)
{
    ClientResponse response;
    auto const parsed = parseUrl(url);
    if (parsed.host.empty())
    {
        response.error = "bad url";
        return response;
    }
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* info = nullptr;
    if (getaddrinfo(parsed.host.c_str(), parsed.port.c_str(), &hints, &info) != 0)
    {
        response.error = "dns";
        return response;
    }
    int fd = -1;
    for (auto* it = info; it != nullptr; it = it->ai_next)
    {
        fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0)
        {
            continue;
        }
        setTimeout(fd, timeout);
        if (::connect(fd, it->ai_addr, it->ai_addrlen) == 0)
        {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(info);
    if (fd < 0)
    {
        response.error = "connect";
        return response;
    }
    std::ostringstream req;
    req << method << " " << parsed.path << " HTTP/1.1\r\n";
    req << "Host: " << parsed.host << "\r\n";
    req << "Accept: application/json\r\n";
    req << "Connection: close\r\n";
    if (!body.empty())
    {
        req << "Content-Type: application/json\r\n";
        req << "Content-Length: " << body.size() << "\r\n";
    }
    req << "\r\n";
    req << body;
    auto const payload = req.str();
    std::size_t sent = 0;
    while (sent < payload.size())
    {
        auto const n = ::send(fd, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (n <= 0)
        {
            ::close(fd);
            response.error = "send";
            return response;
        }
        sent += static_cast<std::size_t>(n);
    }
    std::string raw;
    char buf[4096];
    while (true)
    {
        auto const n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0)
        {
            response.error = "recv";
            break;
        }
        if (n == 0)
        {
            break;
        }
        raw.append(buf, buf + n);
        if (raw.size() > 8 * 1024 * 1024)
        {
            response.error = "too large";
            break;
        }
    }
    ::close(fd);
    auto const headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
    {
        if (response.error.empty())
        {
            response.error = "short response";
        }
        return response;
    }
    auto const head = raw.substr(0, headerEnd);
    response.body = raw.substr(headerEnd + 4);
    std::istringstream lines(head);
    std::string statusLine;
    std::getline(lines, statusLine);
    if (!statusLine.empty() && statusLine.back() == '\r')
    {
        statusLine.pop_back();
    }
    auto const sp1 = statusLine.find(' ');
    if (sp1 != std::string::npos)
    {
        response.status = std::atoi(statusLine.c_str() + sp1 + 1);
    }
    std::string line;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        auto const colon = line.find(':');
        if (colon == std::string::npos)
        {
            continue;
        }
        auto key = line.substr(0, colon);
        auto value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ')
        {
            value.erase(value.begin());
        }
        for (auto& c : key)
        {
            if (c >= 'A' && c <= 'Z')
            {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        response.headers[key] = value;
    }
    return response;
}
} // namespace mfa
