#pragma once

#include <chrono>
#include <map>
#include <string>

namespace mfa
{
struct ClientResponse
{
    int status = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    std::string error;
};

ClientResponse httpRequest(std::string const& method, std::string const& url, std::string const& body = {},
    std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));
} // namespace mfa
