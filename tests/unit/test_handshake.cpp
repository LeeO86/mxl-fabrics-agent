#include <doctest/doctest.h>

#include "replication/handshake.hpp"

using namespace mfa;

PostRequest req(std::string dest, std::string info)
{
    PostRequest request;
    request.domain_id = "dom";
    request.flow_id = "flow";
    request.dest_host_id = dest;
    request.target_info = info;
    request.local_addr = "192.168.12.1";
    request.provider = "tcp";
    return request;
}

TEST_CASE("handshake is idempotent and replaces target info")
{
    HandshakeTable table;
    auto first = table.post(req("node-b", "info-1"));
    CHECK(first.created);
    CHECK(first.state == "pending");
    auto again = table.post(req("node-b", "info-1"));
    CHECK_FALSE(again.created);
    CHECK_FALSE(again.replaced);
    CHECK(again.replication_id == first.replication_id);
    auto replaced = table.post(req("node-b", "info-2"));
    CHECK(replaced.replaced);
    CHECK(replaced.replication_id == first.replication_id);
    auto row = table.find(first.replication_id);
    REQUIRE(row.has_value());
    CHECK(row->targets.size() == 1);
    CHECK(row->targets[0].target_info == "info-2");
    CHECK(row->targets[0].state == "pending");
}

TEST_CASE("same initiator fans out to a second destination")
{
    HandshakeTable table;
    auto first = table.post(req("node-b", "b"));
    auto second = table.post(req("node-c", "c"));
    CHECK(second.replication_id == first.replication_id);
    CHECK(table.find(first.replication_id)->targets.size() == 2);
    CHECK(table.eraseTarget(first.replication_id, "node-b"));
    CHECK(table.find(first.replication_id)->targets.size() == 1);
    CHECK(table.eraseTarget(first.replication_id, "node-c"));
    CHECK_FALSE(table.find(first.replication_id).has_value());
}

TEST_CASE("a different local address is a different initiator")
{
    HandshakeTable table;
    auto first = table.post(req("node-b", "b"));
    auto other = req("node-c", "c");
    other.local_addr = "192.168.13.1";
    auto second = table.post(other);
    CHECK(second.replication_id != first.replication_id);
}
