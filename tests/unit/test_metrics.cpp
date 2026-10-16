#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "fabric/qpsettings.hpp"
#include "ops/lanemetrics.hpp"
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

namespace
{
LaneStats laneWith(int transfers, int stalls, int gaps, std::uint64_t late)
{
    LaneStats lane;
    lane.flowId = "f1";
    lane.provider = "verbs";
    auto& transfer = lane.histograms[static_cast<std::size_t>(LaneMetric::GrainTransfer)];
    transfer.bounds = defaultBounds();
    for (int i = 0; i < transfers; ++i)
    {
        transfer.observe(0.002);
    }
    auto& gap = lane.histograms[static_cast<std::size_t>(LaneMetric::DeliveryGap)];
    gap.bounds = deliveryGapBounds();
    for (int i = 0; i < gaps; ++i)
    {
        gap.observe(0.003);
    }
    lane.lateDeliveries = late;
    for (int i = 0; i < stalls; ++i)
    {
        auto& stall = lane.stalls["pass"];
        stall.bounds = stallBounds();
        stall.observe(0.03);
    }
    return lane;
}

bool has(std::string const& text, std::string const& line)
{
    return text.find(line + "\n") != std::string::npos;
}
} // namespace

TEST_CASE("summed lane series never decrease when a lane goes")
{
    LaneMetrics lanes;
    Metrics m;
    lanes.render(m, {laneWith(3, 1, 5, 2)}, false);
    auto text = m.render();
    CHECK(has(text, "mxl_fabrics_agent_grain_transfer_seconds_count{provider=\"verbs\"} 3"));
    CHECK(has(text, "mxl_fabrics_agent_replication_late_deliveries_total 2"));
    // The lane goes with one more transfer counted since: its final stats are kept.
    lanes.retire({laneWith(4, 1, 6, 2)}, false);
    lanes.render(m, {}, false);
    text = m.render();
    CHECK(has(text, "mxl_fabrics_agent_grain_transfer_seconds_count{provider=\"verbs\"} 4"));
    CHECK(has(text, "mxl_fabrics_agent_fabric_thread_stall_seconds_count{task=\"pass\"} 1"));
    CHECK(has(text, "mxl_fabrics_agent_replication_delivery_gap_seconds_count 6"));
    CHECK(has(text, "mxl_fabrics_agent_replication_late_deliveries_total 2"));
    // A new lane for the flow adds to the total.
    lanes.render(m, {laneWith(1, 0, 1, 1)}, false);
    text = m.render();
    CHECK(has(text, "mxl_fabrics_agent_grain_transfer_seconds_count{provider=\"verbs\"} 5"));
    CHECK(has(text, "mxl_fabrics_agent_replication_late_deliveries_total 3"));
}

TEST_CASE("per-flow lane series go with their lane, the summed ones stay")
{
    LaneMetrics lanes;
    Metrics m;
    lanes.render(m, {laneWith(3, 0, 5, 1)}, true);
    CHECK(has(m.render(), "mxl_fabrics_agent_replication_delivery_gap_seconds_count{flow_id=\"f1\"} 5"));
    lanes.retire({laneWith(3, 0, 5, 1)}, true);
    lanes.render(m, {}, true);
    auto const text = m.render();
    CHECK(text.find("flow_id=\"f1\"") == std::string::npos);
    CHECK(has(text, "mxl_fabrics_agent_grain_transfer_seconds_count{provider=\"verbs\"} 3"));
}

TEST_CASE("a discrete flow is late after more than two grain periods, at least 10 ms")
{
    CHECK_FALSE(lateGrainGap(0.020, 0.020)); // 50p, one period
    CHECK_FALSE(lateGrainGap(0.040, 0.020)); // two periods: not yet
    CHECK(lateGrainGap(0.041, 0.020));
    CHECK_FALSE(lateGrainGap(0.080, 0.040)); // 25p
    CHECK(lateGrainGap(0.081, 0.040));
    CHECK_FALSE(lateGrainGap(0.009, 0.002)); // 500 grains/s: the 10 ms floor
    CHECK(lateGrainGap(0.011, 0.002));
    CHECK(lateGrainGap(0.54, 0.020)); // a retransmission
}

namespace
{
// Counts the late gaps of a sequence as the destination does (pumpTarget).
int lateCount(std::vector<double> const& gaps)
{
    GapWindow window;
    int late = 0;
    for (auto const gap : gaps)
    {
        if (window.usual() > 0 && lateGap(gap, window.usual()))
        {
            ++late;
        }
        window.add(gap);
    }
    return late;
}
} // namespace

TEST_CASE("the usual gap of a continuous flow keeps a regular long gap, not a single stall")
{
    // Bursts: four transfers 3 ms apart, then 12 ms to the origin's next commit (25 ms cycle). The 1.7.2 moving
    // average (about 5 ms) took every 12 ms gap for late: 40 a second.
    std::vector<double> bursts;
    for (int cycle = 0; cycle < 400; ++cycle)
    {
        for (auto const gap : {0.003, 0.003, 0.003, 0.012})
        {
            bursts.push_back(gap);
        }
    }
    CHECK(lateCount(bursts) == 0);
    {
        double usual = 0;
        int late = 0;
        for (auto const gap : bursts)
        {
            if (usual > 0 && lateGap(gap, usual))
            {
                ++late;
            }
            usual = usual > 0 ? usual * 0.98 + std::min(gap, 4.0 * usual) * 0.02 : gap;
        }
        CHECK(late > 300); // the 1.7.2 rule on the same gaps
    }
    // Steady 3 ms deliveries with a 0.54 s retransmission stall now and then: each stall counts once.
    std::vector<double> steady(2000, 0.003);
    steady[500] = 0.54;
    steady[1200] = 0.54;
    steady[1201] = 0.02; // a 20 ms gap right after: over 10 ms, late as well
    auto const late = lateCount(steady);
    CHECK(late == 3);
    // Nothing before 32 gaps.
    CHECK(lateCount({0.003, 0.5}) == 0);
    // The limit: up to 6 long gaps per 256 still count; more often they become the usual gap.
    std::vector<double> rare(2560, 0.003);
    for (std::size_t i = 300; i < rare.size(); i += 64) // 4 per 256
    {
        rare[i] = 0.054;
    }
    CHECK(lateCount(rare) == 36);
    std::vector<double> frequent(2560, 0.003);
    std::size_t stalls = 0;
    for (std::size_t i = 300; i < frequent.size(); i += 16) // 16 per 256
    {
        frequent[i] = 0.054;
        ++stalls;
    }
    CHECK(lateCount(frequent) < stalls / 4);
}

TEST_CASE("transfer stalls are counted per flow and summed without the flow label")
{
    LaneStats lane;
    lane.flowId = "a1";
    lane.provider = "verbs";
    auto& sample = lane.histograms[static_cast<std::size_t>(LaneMetric::SampleTransfer)];
    sample.bounds = sampleTransferBounds();
    sample.observe(0.003);
    sample.observe(0.54);
    lane.sampleTransferStalls = 1;
    LaneStats video;
    video.flowId = "v1";
    video.provider = "verbs";
    auto& grain = video.histograms[static_cast<std::size_t>(LaneMetric::GrainTransfer)];
    grain.bounds = defaultBounds();
    grain.observe(0.6);
    video.grainTransferStalls = 1;
    video.deliveries = 0;
    LaneMetrics lanes;
    Metrics m;
    lanes.render(m, {lane, video}, true);
    auto text = m.render();
    CHECK(has(text, "mxl_fabrics_agent_sample_transfer_stalls_total{flow_id=\"a1\",provider=\"verbs\"} 1"));
    CHECK(has(text, "mxl_fabrics_agent_grain_transfer_stalls_total{provider=\"verbs\"} 1"));
    lanes.retire({lane, video}, false);
    lanes.render(m, {}, false);
    text = m.render();
    CHECK(has(text, "mxl_fabrics_agent_sample_transfer_stalls_total{provider=\"verbs\"} 1"));
    CHECK(has(text, "mxl_fabrics_agent_grain_transfer_stalls_total{provider=\"verbs\"} 1"));
}

TEST_CASE("a discrete destination reports late deliveries without a delivery gap histogram")
{
    LaneStats video;
    video.flowId = "v1";
    video.deliveries = 100;
    video.lateDeliveries = 2;
    LaneMetrics lanes;
    Metrics m;
    lanes.render(m, {video}, true);
    auto const text = m.render();
    CHECK(has(text, "mxl_fabrics_agent_replication_late_deliveries_total{flow_id=\"v1\"} 2"));
    CHECK(text.find("replication_delivery_gap_seconds_count{flow_id=\"v1\"}") == std::string::npos);
}

TEST_CASE("ACK timeout: 4.096 us x 2^n; the stall threshold is fixed")
{
    CHECK(qp::ackTimeoutSeconds(14) == doctest::Approx(0.0671088));
    CHECK(qp::ackTimeoutSeconds(17) == doctest::Approx(0.5368709));
    CHECK(qp::ackTimeoutSeconds(4) == doctest::Approx(65.536e-6));
    // Left to rdma_cm: RoCE's 17.
    CHECK(qp::effectiveAckTimeout({0, 7}) == 17);
    // Catches irdma's retransmission with roce_rtomin 5 (about 54 ms) and mlx5's with the default 14 (67 ms).
    for (auto const threshold : {qp::kSampleTransferStallSeconds, qp::kGrainTransferStallSeconds})
    {
        CHECK(threshold < 0.054);
        CHECK(threshold < qp::ackTimeoutSeconds(qp::kDefaultAckTimeout));
    }
}
