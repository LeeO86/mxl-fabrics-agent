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
    /// Replaces every series of a gauge (labels that are gone, such as a former state, disappear).
    void setGauges(std::string const& name, std::vector<std::pair<std::map<std::string, std::string>, double>> const& series);
    void addCounter(std::string const& name, double value, std::map<std::string, std::string> const& labels = {});
    /// A counter whose total is kept elsewhere (a replication's grains, bytes, errors, restarts).
    void setCounter(std::string const& name, double value, std::map<std::string, std::string> const& labels = {});
    void observe(std::string const& name, double seconds, std::map<std::string, std::string> const& labels = {});
    /// The same with the histogram's own bucket bounds (ascending; the first observation of a series sets them).
    void observe(std::string const& name, double seconds, std::map<std::string, std::string> const& labels,
        std::vector<double> const& bounds);
    /// A gauge of the largest value since the last render (a scrape): render shows it and starts it again at 0.
    void observeMax(std::string const& name, double value, std::map<std::string, std::string> const& labels = {});
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
    mutable std::vector<Series> maxes_;
};
} // namespace mfa
