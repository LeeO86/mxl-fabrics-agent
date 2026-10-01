#pragma once

#include "config/config.hpp"

#include <functional>
#include <string>

namespace mfa
{
class NmosNode
{
public:
    using Ready = std::function<void(bool, std::string const&)>;

    NmosNode(Config cfg, std::string nodeId, Ready ready);
    ~NmosNode();
    void start();
    void stop();
    std::string id() const { return nodeId_; }

private:
    Config cfg_;
    std::string nodeId_;
    Ready ready_;
    struct Impl;
    Impl* impl_ = nullptr;
};
} // namespace mfa
