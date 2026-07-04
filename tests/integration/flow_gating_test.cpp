#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "roqr/quic/client.hpp"
#include "roqr/relayd/server.hpp"

using namespace std::chrono_literals;

namespace {
const std::string kCertDir = ROQR_TEST_CERT_DIR;

struct Collector {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<roqr::Frame> frames;
    void add(const roqr::Frame& f) {
        std::lock_guard lock(mutex);
        frames.push_back(f);
        cv.notify_all();
    }
    bool wait_count(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return frames.size() >= n; });
    }
    size_t count() {
        std::lock_guard lock(mutex);
        return frames.size();
    }
};

roqr::Frame flow_frame(uint64_t flow_id, uint64_t ts) {
    roqr::Frame f;
    f.flow_id = flow_id;
    f.timestamp = ts;
    f.message_type = 9;
    f.message_stream_id = 1;
    f.chunk_stream_id = 4;
    f.payload = {0x0F};
    return f;
}
}  // namespace

TEST_CASE("frames for an unbound flow buffer until bind_flow") {
    roqr::relayd::Server server;
    roqr::relayd::ServerOptions so;
    so.port = 45557;
    so.cert_file = kCertDir + "/cert.pem";
    so.key_file = kCertDir + "/key.pem";
    REQUIRE(server.start(so));

    Collector got;
    roqr::quic::Client client;
    client.on_message([&](const roqr::Frame& f) { got.add(f); });
    REQUIRE(client.connect("127.0.0.1", 45557));
    REQUIRE(client.wait_connected(5s));

    // Echoed back with flow 7, which this client never bound for receive.
    REQUIRE(client.send(flow_frame(7, 1), roqr::quic::DeliveryMode::Stream));
    // Flow 0 frame proves the echo round-trip completed while flow 7 waits.
    REQUIRE(client.send(flow_frame(0, 2), roqr::quic::DeliveryMode::Stream));
    REQUIRE(got.wait_count(1, 5s));
    CHECK(got.frames[0].flow_id == 0);
    CHECK(got.count() == 1);  // flow 7 buffered, not delivered

    client.bind_flow(7);
    REQUIRE(got.wait_count(2, 5s));
    {
        std::lock_guard lock(got.mutex);
        CHECK(got.frames[1].flow_id == 7);
        CHECK(got.frames[1].timestamp == 1);
    }
    client.close();
    client.wait_closed(5s);
    server.stop();
}

TEST_CASE("frames for a retired flow are dropped") {
    roqr::relayd::Server server;
    roqr::relayd::ServerOptions so;
    so.port = 45558;
    so.cert_file = kCertDir + "/cert.pem";
    so.key_file = kCertDir + "/key.pem";
    REQUIRE(server.start(so));

    Collector got;
    roqr::quic::Client client;
    client.on_message([&](const roqr::Frame& f) { got.add(f); });
    REQUIRE(client.connect("127.0.0.1", 45558));
    REQUIRE(client.wait_connected(5s));

    client.bind_flow(9);
    client.retire_flow(9);
    REQUIRE(client.send(flow_frame(9, 1), roqr::quic::DeliveryMode::Stream));
    REQUIRE(client.send(flow_frame(0, 2), roqr::quic::DeliveryMode::Stream));
    REQUIRE(got.wait_count(1, 5s));
    // Only the flow 0 frame arrives; the retired flow 9 echo was dropped.
    CHECK(got.frames[0].flow_id == 0);
    CHECK(got.count() == 1);

    client.close();
    client.wait_closed(5s);
    server.stop();
}
