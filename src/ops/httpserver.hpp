#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace mfa
{
struct HttpRequest
{
    std::string method;
    std::string path;
    std::string query;
    std::string body;
    std::map<std::string, std::string> headers;
};

struct HttpResponse
{
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
    bool sse = false;
};

using HttpHandler = std::function<HttpResponse(HttpRequest const&)>;
using SseEmit = std::function<bool(std::string const& event, std::string const& data)>;
using SseHandler = std::function<void(SseEmit const&)>;

class HttpServer
{
public:
    HttpServer();
    ~HttpServer();
    void setHandler(HttpHandler handler);
    void setSseHandler(SseHandler handler);
    bool start(int port);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mfa
