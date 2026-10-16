#pragma once

#include "fabric/lanestats.hpp"

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
    /// Replaces every series of a gauge (labels that are gone, such as a former state, disappear).
    void setGauges(std::string const& name, std::vector<std::pair<std::map<std::string, std::string>, double>> const& series);
    void addCounter(std::string const& name, double value, std::map<std::string, std::string> const& labels = {});
    /// A counter whose total is kept elsewhere (a replication's grains, bytes, errors, restarts).
    void setCounter(std::string const& name, double value, std::map<std::string, std::string> const& labels = {});
    void observe(std::string const& name, double seconds, std::map<std::string, std::string> const& labels = {});
    /// The same with the histogram's own bucket bounds (ascending; the first observation of a series sets them).
    void observe(std::string const& name, double seconds, std::map<std::string, std::string> const& labels,
        std::vector<double> const& bounds);
    /// Replaces every series of a histogram counted elsewhere (the lanes); series that are gone disappear.
    void setHistograms(std::string const& name, std::vector<std::pair<std::map<std::string, std::string>, LaneHistogram>> const& series);
    /// The same for a counter kept elsewhere.
    void setCounters(std::string const& name, std::vector<std::pair<std::map<std::string, std::string>, double>> const& series);
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
        std::vector<double> bounds;
        std::vector<std::uint64_t> buckets;
    };
    mutable std::mutex mu_;
    std::vector<Series> gauges_;
    std::vector<Series> counters_;
    std::vector<Hist> hists_;
};
} // namespace mfa
