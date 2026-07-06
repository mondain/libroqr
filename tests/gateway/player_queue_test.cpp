#include <catch2/catch_test_macros.hpp>

#include <thread>

#include "roqr/gateway/player_queue.hpp"

using namespace roqr::gateway;
using roqr::rtmp::RtmpMessage;

namespace {
RtmpMessage vid(uint32_t ts) {
    RtmpMessage m;
    m.type = 9;
    m.timestamp = ts;
    m.payload = {0x27, 0x01};
    return m;
}
RtmpMessage seq_header() {
    RtmpMessage m;
    m.type = 9;
    m.payload = {0x17, 0x00};
    return m;
}
}  // namespace

TEST_CASE("queue is FIFO under the bound") {
    PlayerQueue q(8);
    CHECK(q.push(vid(1), PlayerQueue::Kind::Coded));
    CHECK(q.push(vid(2), PlayerQueue::Kind::Coded));
    CHECK(q.size() == 2);
    CHECK(q.pop()->timestamp == 1);
    CHECK(q.pop()->timestamp == 2);
}

TEST_CASE("overflow drops the oldest coded frame, stays bounded") {
    PlayerQueue q(4);
    for (uint32_t i = 0; i < 4; ++i) {
        REQUIRE(q.push(vid(i), PlayerQueue::Kind::Coded));
    }
    // Now full. Pushing more evicts the oldest coded frame each time.
    q.push(vid(100), PlayerQueue::Kind::Coded);
    q.push(vid(101), PlayerQueue::Kind::Coded);
    CHECK(q.size() == 4);          // bounded
    CHECK(q.dropped() == 2);       // two evictions
    // The two oldest (ts 0,1) were evicted; front is now ts 2.
    CHECK(q.pop()->timestamp == 2);
}

TEST_CASE("sequence headers are never evicted") {
    PlayerQueue q(3);
    REQUIRE(q.push(seq_header(), PlayerQueue::Kind::Init));
    REQUIRE(q.push(vid(1), PlayerQueue::Kind::Coded));
    REQUIRE(q.push(vid(2), PlayerQueue::Kind::Coded));
    // Full. Two more coded frames evict the coded ones, keeping the Init.
    q.push(vid(3), PlayerQueue::Kind::Coded);
    q.push(vid(4), PlayerQueue::Kind::Coded);
    CHECK(q.size() == 3);
    // The Init (seq header) is still at the front.
    auto first = q.pop();
    REQUIRE(first.has_value());
    CHECK(first->payload == std::vector<uint8_t>{0x17, 0x00});
}

TEST_CASE("close unblocks a waiting consumer and drains remaining") {
    PlayerQueue q(8);
    q.push(vid(1), PlayerQueue::Kind::Coded);
    q.close();
    CHECK(q.pop()->timestamp == 1);  // drains the queued item
    CHECK_FALSE(q.pop().has_value()); // then reports closed+empty

    PlayerQueue q2(8);
    std::thread consumer([&] {
        auto m = q2.pop();  // blocks until close()
        CHECK_FALSE(m.has_value());
    });
    q2.close();
    consumer.join();
}
