#include "ops/metrics.hpp"

#include <algorithm>
#include <set>
#include <sstream>

namespace mfa
{
namespace
{
std::vector<double> const& kBuckets = defaultBounds();

std::string boundText(double bound)
{
    std::ostringstream out;
    out << bound;
    return out.str();
}

std::string labelsOf(std::map<std::string, std::string> const& labels)
{
    if (labels.empty())
    {
        return "";
    }
    std::string out = "{";
    bool first = true;
    for (auto const& [key, value] : labels)
    {
        if (!first)
        {
            out += ",";
        }
        first = false;
        out += key;
        out += "=\"";
        for (char c : value)
        {
            if (c == '\\' || c == '"')
            {
                out += '\\';
            }
            if (c == '\n')
            {
                out += "\\n";
            }
            else
            {
                out += c;
            }
        }
        out += "\"";
    }
    out += "}";
    return out;
}

template <typename T>
T* find(std::vector<T>& rows, std::string const& name, std::map<std::string, std::string> const& labels)
{
    for (auto& row : rows)
    {
        if (row.name == name && row.labels == labels)
        {
            return &row;
        }
    }
    return nullptr;
}
} // namespace

void Metrics::setGauge(std::string const& name, double value, std::map<std::string, std::string> const& labels)
{
    std::lock_guard const lock{mu_};
    if (auto* row = find(gauges_, name, labels))
    {
        row->value = value;
        return;
    }
    gauges_.push_back(Series{name, labels, value});
}

void Metrics::setGauges(std::string const& name, std::vector<std::pair<std::map<std::string, std::string>, double>> const& series)
{
    std::lock_guard const lock{mu_};
    std::erase_if(gauges_, [&](Series const& row) { return row.name == name; });
    for (auto const& [labels, value] : series)
    {
        gauges_.push_back(Series{name, labels, value});
    }
}

void Metrics::addCounter(std::string const& name, double value, std::map<std::string, std::string> const& labels)
{
    std::lock_guard const lock{mu_};
    if (auto* row = find(counters_, name, labels))
    {
        row->value += value;
        return;
    }
    counters_.push_back(Series{name, labels, value});
}

void Metrics::setCounter(std::string const& name, double value, std::map<std::string, std::string> const& labels)
{
    std::lock_guard const lock{mu_};
    if (auto* row = find(counters_, name, labels))
    {
        row->value = value;
        return;
    }
    counters_.push_back(Series{name, labels, value});
}

void Metrics::observe(std::string const& name, double seconds, std::map<std::string, std::string> const& labels)
{
    observe(name, seconds, labels, kBuckets);
}

void Metrics::observe(std::string const& name, double seconds, std::map<std::string, std::string> const& labels,
    std::vector<double> const& bounds)
{
    std::lock_guard const lock{mu_};
    Hist* row = find(hists_, name, labels);
    if (row == nullptr)
    {
        hists_.push_back(Hist{name, labels, 0, 0, bounds, std::vector<std::uint64_t>(bounds.size(), 0)});
        row = &hists_.back();
    }
    row->count += 1;
    row->sum += seconds;
    for (std::size_t i = 0; i < row->bounds.size(); ++i)
    {
        if (seconds <= row->bounds[i])
        {
            row->buckets[i] += 1;
        }
    }
}

void Metrics::setHistograms(std::string const& name, std::vector<std::pair<std::map<std::string, std::string>, LaneHistogram>> const& series)
{
    std::lock_guard const lock{mu_};
    hists_.erase(std::remove_if(hists_.begin(), hists_.end(), [&](Hist const& row) { return row.name == name; }), hists_.end());
    for (auto const& [labels, histogram] : series)
    {
        auto buckets = histogram.buckets;
        buckets.resize(histogram.bounds.size(), 0);
        hists_.push_back(Hist{name, labels, histogram.count, histogram.sum, histogram.bounds, std::move(buckets)});
    }
}

void Metrics::setCounters(std::string const& name, std::vector<std::pair<std::map<std::string, std::string>, double>> const& series)
{
    std::lock_guard const lock{mu_};
    counters_.erase(std::remove_if(counters_.begin(), counters_.end(), [&](Series const& row) { return row.name == name; }), counters_.end());
    for (auto const& [labels, value] : series)
    {
        counters_.push_back(Series{name, labels, value});
    }
}

std::string Metrics::render() const
{
    std::lock_guard const lock{mu_};
    std::ostringstream out;
    std::set<std::string> seen;
    auto typeOnce = [&](std::string const& metric, char const* kind) {
        if (seen.insert(metric).second)
        {
            out << "# TYPE " << metric << " " << kind << "\n";
        }
    };
    for (auto const& row : gauges_)
    {
        auto const metric = "mxl_fabrics_agent_" + row.name;
        typeOnce(metric, "gauge");
        out << metric << labelsOf(row.labels) << " " << row.value << "\n";
    }
    seen.clear();
    for (auto const& row : counters_)
    {
        auto const metric = "mxl_fabrics_agent_" + row.name;
        typeOnce(metric, "counter");
        out << metric << labelsOf(row.labels) << " " << row.value << "\n";
    }
    seen.clear();
    for (auto const& row : hists_)
    {
        auto const metric = "mxl_fabrics_agent_" + row.name;
        typeOnce(metric, "histogram");
        for (std::size_t i = 0; i < row.bounds.size(); ++i)
        {
            auto labels = row.labels;
            labels["le"] = row.bounds == kBuckets ? std::to_string(row.bounds[i]) : boundText(row.bounds[i]);
            out << metric << "_bucket" << labelsOf(labels) << " " << row.buckets[i] << "\n";
        }
        auto inf = row.labels;
        inf["le"] = "+Inf";
        out << metric << "_bucket" << labelsOf(inf) << " " << row.count << "\n";
        out << metric << "_sum" << labelsOf(row.labels) << " " << row.sum << "\n";
        out << metric << "_count" << labelsOf(row.labels) << " " << row.count << "\n";
    }
    return out.str();
}
} // namespace mfa
