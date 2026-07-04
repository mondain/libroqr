#include <catch2/catch_test_macros.hpp>

#include <thread>

#include "roqr/quic/outbound_queue.hpp"

using namespace roqr::quic;

namespace {
roqr::Frame make_frame(uint64_t ts) {
    roqr::Frame f;
    f.message_type = 9;
    f.timestamp = ts;
    f.payload = {0x01};
    return f;
}
}  // namespace

TEST_CASE("queue is FIFO") {
    OutboundQueue q;
    q.push({make_frame(1), DeliveryMode::Stream});
    q.push({make_frame(2), DeliveryMode::Datagram});
    auto a = q.pop();
    auto b = q.pop();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->frame.timestamp == 1);
    CHECK(a->mode == DeliveryMode::Stream);
    CHECK(b->frame.timestamp == 2);
    CHECK_FALSE(q.pop().has_value());
}

TEST_CASE("concurrent producers do not lose items") {
    OutboundQueue q;
    constexpr int kPerThread = 500;
    std::thread t1([&] {
        for (int i = 0; i < kPerThread; ++i)
            q.push({make_frame(1), DeliveryMode::Stream});
    });
    std::thread t2([&] {
        for (int i = 0; i < kPerThread; ++i)
            q.push({make_frame(2), DeliveryMode::Stream});
    });
    t1.join();
    t2.join();
    CHECK(q.size() == 2 * kPerThread);
}
