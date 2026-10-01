#include "ops/metrics.hpp"

#include <set>
#include <sstream>

namespace mfa
{
namespace
{
constexpr double kBuckets[] = {0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 5.0};

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

void Metrics::observe(std::string const& name, double seconds, std::map<std::string, std::string> const& labels)
{
    std::lock_guard const lock{mu_};
    Hist* row = find(hists_, name, labels);
    if (row == nullptr)
    {
        hists_.push_back(Hist{name, labels, 0, 0, {}});
        row = &hists_.back();
    }
    row->count += 1;
    row->sum += seconds;
    for (int i = 0; i < 8; ++i)
    {
        if (seconds <= kBuckets[i])
        {
            row->buckets[i] += 1;
        }
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
        for (int i = 0; i < 8; ++i)
        {
            auto labels = row.labels;
            labels["le"] = std::to_string(kBuckets[i]);
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
