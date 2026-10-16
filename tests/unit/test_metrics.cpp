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

TEST_CASE("setHistograms replaces the series of a histogram counted elsewhere")
{
    Metrics m;
    LaneHistogram h;
    h.bounds = {0.01, 0.1};
    h.observe(0.005);
    h.observe(0.2);
    m.setHistograms("replication_delivery_gap_seconds", {{{{"flow_id", "a"}}, h}, {{{"flow_id", "b"}}, h}});
    m.setHistograms("replication_delivery_gap_seconds", {{{{"flow_id", "b"}}, h}});
    auto const text = m.render();
    CHECK(text.find("flow_id=\"a\"") == std::string::npos);
    CHECK(text.find("mxl_fabrics_agent_replication_delivery_gap_seconds_bucket{flow_id=\"b\",le=\"0.01\"} 1\n") != std::string::npos);
    CHECK(text.find("mxl_fabrics_agent_replication_delivery_gap_seconds_count{flow_id=\"b\"} 2\n") != std::string::npos);
}

TEST_CASE("the rolling maximum covers the last 10 s and is not reset by reading")
{
    RollingMax max;
    max.observe(0.208, 100);
    max.observe(0.004, 101);
    CHECK(max.value(101) == doctest::Approx(0.208));
    CHECK(max.value(101) == doctest::Approx(0.208)); // a second scraper sees the same
    CHECK(max.value(109) == doctest::Approx(0.208));
    CHECK(max.value(110) == doctest::Approx(0.004));
    CHECK(max.value(111) == doctest::Approx(0.0));
    max.observe(0.05, 120); // the slot of second 100 is taken again
    CHECK(max.value(120) == doctest::Approx(0.05));
}

TEST_CASE("a late delivery is a gap of more than twice the usual one, at least 10 ms")
{
    CHECK_FALSE(lateGap(0.006, 0.003));
    CHECK(lateGap(0.011, 0.003));
    CHECK_FALSE(lateGap(0.035, 0.02));
    CHECK(lateGap(0.041, 0.02));
}

TEST_CASE("lane histograms merge only with the same bounds")
{
    LaneHistogram a;
    a.bounds = {0.01};
    a.observe(0.001);
    LaneHistogram b = a;
    b.observe(0.5);
    a.merge(b);
    CHECK(a.count == 3);
    CHECK(a.buckets[0] == 2);
    LaneHistogram other;
    other.bounds = {0.02};
    other.observe(0.001);
    a.merge(other);
    CHECK(a.count == 3);
}
