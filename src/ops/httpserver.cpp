#include "ops/httpserver.hpp"

#include "util/logging.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <thread>

namespace mfa
{
namespace
{
bool sendAll(int fd, std::string const& data)
{
    std::size_t sent = 0;
    while (sent < data.size())
    {
        auto const n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0)
        {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::string statusText(int status)
{
    switch (status)
    {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "OK";
    }
}
} // namespace

struct HttpServer::Impl
{
    HttpHandler handler;
    SseHandler sse;
    int listen = -1;
    int wake[2] = {-1, -1};
    std::atomic<bool> run{false};
    std::thread thread;

    void acceptLoop()
    {
        while (run.load())
        {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(listen, &fds);
            int maxfd = listen;
            if (wake[0] >= 0)
            {
                FD_SET(wake[0], &fds);
                if (wake[0] > maxfd)
                {
                    maxfd = wake[0];
                }
            }
            timeval tv{};
            tv.tv_sec = 1;
            auto const rc = ::select(maxfd + 1, &fds, nullptr, nullptr, &tv);
            if (rc < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                break;
            }
            if (wake[0] >= 0 && FD_ISSET(wake[0], &fds))
            {
                char buf[32];
                ::recv(wake[0], buf, sizeof(buf), 0);
                break;
            }
            if (!FD_ISSET(listen, &fds))
            {
                continue;
            }
            int fd = ::accept(listen, nullptr, nullptr);
            if (fd < 0)
            {
                continue;
            }
            std::thread(&Impl::serve, this, fd).detach();
        }
    }

    void serve(int fd)
    {
        std::string raw;
        char buf[4096];
        while (raw.find("\r\n\r\n") == std::string::npos)
        {
            auto const n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
            {
                ::close(fd);
                return;
            }
            raw.append(buf, buf + n);
            if (raw.size() > 2 * 1024 * 1024)
            {
                ::close(fd);
                return;
            }
        }
        auto const headerEnd = raw.find("\r\n\r\n");
        auto const head = raw.substr(0, headerEnd);
        std::string body = raw.substr(headerEnd + 4);
        std::istringstream lines(head);
        std::string requestLine;
        std::getline(lines, requestLine);
        if (!requestLine.empty() && requestLine.back() == '\r')
        {
            requestLine.pop_back();
        }
        HttpRequest req;
        {
            std::istringstream rl(requestLine);
            std::string target;
            rl >> req.method >> target;
            auto const q = target.find('?');
            req.path = q == std::string::npos ? target : target.substr(0, q);
            req.query = q == std::string::npos ? "" : target.substr(q + 1);
        }
        std::size_t contentLength = 0;
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
            req.headers[key] = value;
            if (key == "content-length")
            {
                contentLength = static_cast<std::size_t>(std::strtoul(value.c_str(), nullptr, 10));
            }
        }
        while (body.size() < contentLength)
        {
            auto const n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
            {
                break;
            }
            body.append(buf, buf + n);
        }
        if (body.size() > contentLength)
        {
            body.resize(contentLength);
        }
        req.body = std::move(body);
        if (req.path == "/api/v1/events" && req.method == "GET" && sse)
        {
            std::string headers = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
            if (!sendAll(fd, headers))
            {
                ::close(fd);
                return;
            }
            sse([&](std::string const& event, std::string const& data) {
                std::string chunk = "event: " + event + "\ndata: " + data + "\n\n";
                return sendAll(fd, chunk);
            });
            ::close(fd);
            return;
        }
        HttpResponse res;
        try
        {
            res = handler ? handler(req) : HttpResponse{404, "application/json", "{\"error\":\"not found\"}", false};
        }
        catch (std::exception const& ex)
        {
            res.status = 500;
            res.content_type = "application/json";
            res.body = std::string("{\"error\":\"") + ex.what() + "\"}";
        }
        std::ostringstream out;
        out << "HTTP/1.1 " << res.status << " " << statusText(res.status) << "\r\n";
        out << "Content-Type: " << res.content_type << "\r\n";
        out << "Content-Length: " << res.body.size() << "\r\n";
        out << "Connection: close\r\n\r\n";
        out << res.body;
        sendAll(fd, out.str());
        ::close(fd);
    }
};

HttpServer::HttpServer()
    : impl_(std::make_unique<Impl>())
{}

HttpServer::~HttpServer()
{
    stop();
}

void HttpServer::setHandler(HttpHandler handler)
{
    impl_->handler = std::move(handler);
}

void HttpServer::setSseHandler(SseHandler handler)
{
    impl_->sse = std::move(handler);
}

bool HttpServer::start(int port)
{
    impl_->listen = ::socket(AF_INET, SOCK_STREAM, 0);
    if (impl_->listen < 0)
    {
        return false;
    }
    int one = 1;
    setsockopt(impl_->listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(impl_->listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || listen(impl_->listen, 64) < 0)
    {
        ::close(impl_->listen);
        impl_->listen = -1;
        return false;
    }
    if (::pipe(impl_->wake) != 0)
    {
        impl_->wake[0] = impl_->wake[1] = -1;
    }
    impl_->run = true;
    impl_->thread = std::thread([this] { impl_->acceptLoop(); });
    log::info("http_listen", {{"port", std::to_string(port)}});
    return true;
}

void HttpServer::stop()
{
    if (!impl_)
    {
        return;
    }
    impl_->run = false;
    if (impl_->wake[1] >= 0)
    {
        char b = 1;
        ::write(impl_->wake[1], &b, 1);
    }
    if (impl_->thread.joinable())
    {
        impl_->thread.join();
    }
    if (impl_->listen >= 0)
    {
        ::close(impl_->listen);
        impl_->listen = -1;
    }
    if (impl_->wake[0] >= 0)
    {
        ::close(impl_->wake[0]);
        ::close(impl_->wake[1]);
        impl_->wake[0] = impl_->wake[1] = -1;
    }
}
} // namespace mfa
