#pragma once

#include "config/store.hpp"
#include "demand/derive.hpp"
#include "inventory/scanner.hpp"
#include "nmos/node.hpp"
#include "nmos/observer.hpp"
#include "ops/httpserver.hpp"
#include "ops/metrics.hpp"
#include "peer/manager.hpp"
#include "replication/engine.hpp"
#include "util/net.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace mfa
{
class Controller
{
public:
    explicit Controller(std::shared_ptr<ConfigStore> store);
    ~Controller();

    void start();
    void stop();
    bool ready() const { return ready_.load(); }
    int fatal() const { return fatal_.load(); }
    HttpResponse handle(HttpRequest const& request);
    void stream(SseEmit const& emit);

private:
    void loop();
    void tick();
    void publish(std::string const& event, std::string const& data);
    void renderMetrics();
    HttpResponse testPeer(std::string const& hostId);

    std::shared_ptr<ConfigStore> store_;
    std::string bootId_;
    ReplicationEngine engine_;
    DomainScanner scanner_;
    PeerManager peers_;
    NmosObserver observer_;
    std::unique_ptr<NmosNode> node_;
    Metrics metrics_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> nmosReady_{false};
    std::atomic<bool> nmosRegistered_{false};
    std::atomic<int> fatal_{0};
    std::string nmosError_;
    DemandSnapshot demand_;
    std::vector<DomainRecord> mirrors_;
    std::map<std::string, std::chrono::steady_clock::time_point> originLost_;
    std::map<std::string, std::chrono::steady_clock::time_point> demandLost_;
    std::set<std::string> lastDemand_;
    std::map<std::string, FabricLink> links_;  // per peer, from the last tick (mu_)
    std::map<std::string, std::string> linkErrors_; // per peer, the last logged link error ("" = up)
    struct OriginConflict
    {
        std::string hosts;  // the peers that hold the flow
        std::string chosen; // the one replicated from
    };
    std::map<std::string, OriginConflict> originConflicts_; // per "domain/flow" held by more than one peer
    struct Event
    {
        std::string name;
        std::string data;
    };
    std::mutex eventMu_;
    std::condition_variable eventCv_;
    std::vector<Event> events_;
    bool eventsStop_ = false;
    int tai_ = 0;
    bool rootTmpfs_ = false;
};
} // namespace mfa
