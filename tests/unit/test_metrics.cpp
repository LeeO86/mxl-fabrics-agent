#include <doctest/doctest.h>

#include <string>

#include "ops/metrics.hpp"

using namespace mfa;

TEST_CASE("setCounter keeps a total counted elsewhere, addCounter adds")
{
    Metrics m;
    m.setCounter("replication_grains_total", 10, {{"flow_id", "f"}, {"role", "destination"}});
    m.setCounter("replication_grains_total", 25, {{"flow_id", "f"}, {"role", "destination"}});
    m.addCounter("demo_total", 2);
    m.addCounter("demo_total", 3);
    auto const text = m.render();
    CHECK(text.find("# TYPE mxl_fabrics_agent_replication_grains_total counter") != std::string::npos);
    CHECK(text.find("mxl_fabrics_agent_replication_grains_total{flow_id=\"f\",role=\"destination\"} 25\n") != std::string::npos);
    CHECK(text.find("mxl_fabrics_agent_demo_total 5\n") != std::string::npos);
}

TEST_CASE("setGauges replaces every series of a gauge")
{
    Metrics m;
    m.setGauges("replication_state", {{{{"flow_id", "f"}, {"state", "active"}}, 1}});
    m.setGauges("replication_state", {{{{"flow_id", "f"}, {"state", "link_down"}}, 1}});
    auto const text = m.render();
    CHECK(text.find("state=\"active\"") == std::string::npos);
    CHECK(text.find("mxl_fabrics_agent_replication_state{flow_id=\"f\",state=\"link_down\"} 1\n") != std::string::npos);
}

TEST_CASE("a histogram with its own buckets; the default buckets render as before")
{
    Metrics m;
    m.observe("replication_sample_age_seconds", 0.15, {{"stage", "delivered"}}, {0.01, 0.12, 0.2});
    m.observe("replication_sample_age_seconds", 0.005, {{"stage", "delivered"}}, {0.01, 0.12, 0.2});
    m.observe("grain_transfer_seconds", 0.002, {{"provider", "tcp"}});
    auto const text = m.render();
    CHECK(text.find("mxl_fabrics_agent_replication_sample_age_seconds_bucket{le=\"0.01\",stage=\"delivered\"} 1\n") != std::string::npos);
    CHECK(text.find("mxl_fabrics_agent_replication_sample_age_seconds_bucket{le=\"0.12\",stage=\"delivered\"} 1\n") != std::string::npos);
    CHECK(text.find("mxl_fabrics_agent_replication_sample_age_seconds_bucket{le=\"0.2\",stage=\"delivered\"} 2\n") != std::string::npos);
    CHECK(text.find("mxl_fabrics_agent_replication_sample_age_seconds_bucket{le=\"+Inf\",stage=\"delivered\"} 2\n") != std::string::npos);
    CHECK(text.find("mxl_fabrics_agent_grain_transfer_seconds_bucket{le=\"0.005000\",provider=\"tcp\"} 1\n") != std::string::npos);
}

TEST_CASE("observeMax shows the largest value since the last render, then starts again")
{
    Metrics m;
    m.observeMax("replication_sample_age_max_seconds", 0.004, {{"flow_id", "f"}});
    m.observeMax("replication_sample_age_max_seconds", 0.208, {{"flow_id", "f"}});
    m.observeMax("replication_sample_age_max_seconds", 0.006, {{"flow_id", "f"}});
    auto const first = m.render();
    CHECK(first.find("# TYPE mxl_fabrics_agent_replication_sample_age_max_seconds gauge") != std::string::npos);
    CHECK(first.find("mxl_fabrics_agent_replication_sample_age_max_seconds{flow_id=\"f\"} 0.208\n") != std::string::npos);
    auto const second = m.render();
    CHECK(second.find("mxl_fabrics_agent_replication_sample_age_max_seconds{flow_id=\"f\"} 0\n") != std::string::npos);
}
