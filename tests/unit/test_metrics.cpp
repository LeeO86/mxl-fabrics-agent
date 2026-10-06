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
