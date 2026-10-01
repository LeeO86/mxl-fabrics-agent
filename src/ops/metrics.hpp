#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace mfa
{
class Metrics
{
public:
    void setGauge(std::string const& name, double value, std::map<std::string, std::string> const& labels = {});
    void addCounter(std::string const& name, double value, std::map<std::string, std::string> const& labels = {});
    void observe(std::string const& name, double seconds, std::map<std::string, std::string> const& labels = {});
    std::string render() const;

private:
    struct Series
    {
        std::string name;
        std::map<std::string, std::string> labels;
        double value = 0;
    };
    struct Hist
    {
        std::string name;
        std::map<std::string, std::string> labels;
        std::uint64_t count = 0;
        double sum = 0;
        std::uint64_t buckets[8]{};
    };
    mutable std::mutex mu_;
    std::vector<Series> gauges_;
    std::vector<Series> counters_;
    std::vector<Hist> hists_;
};
} // namespace mfa
