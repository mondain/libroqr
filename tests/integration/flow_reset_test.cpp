#include <catch2/catch_test_macros.hpp>

#include <atomic>
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

TEST_CASE("reset_flow_stream cancels a flow stream without killing the connection") {
    roqr::relayd::Server server;
    roqr::relayd::ServerOptions so;
    so.port = 45560;
    so.cert_file = kCertDir + "/cert.pem";
    so.key_file = kCertDir + "/key.pem";
    REQUIRE(server.start(so));

    Collector got;
    roqr::quic::Client client;
    client.on_message([&](const roqr::Frame& f) { got.add(f); });
    REQUIRE(client.connect("127.0.0.1", 45560));
    REQUIRE(client.wait_connected(5s));

    // Establish a send stream for flow 3, then cancel it.
    client.bind_flow(3);
    REQUIRE(client.send(flow_frame(3, 1), roqr::quic::DeliveryMode::Stream));
    REQUIRE(got.wait_count(1, 5s));
    client.reset_flow_stream(3);

    // A further flow 3 send must still work: the client allocates a fresh
    // stream after reset (reset only cancels the old stream).
    REQUIRE(client.send(flow_frame(3, 9), roqr::quic::DeliveryMode::Stream));
    REQUIRE(got.wait_count(2, 5s));

    // Connection survives: flow 0 traffic still round-trips afterwards.
    REQUIRE(client.send(flow_frame(0, 2), roqr::quic::DeliveryMode::Stream));
    REQUIRE(got.wait_count(3, 5s));

    client.close(roqr::ErrorCode::NoError);
    CHECK(client.wait_closed(5s));
    server.stop();
}

TEST_CASE("reset_flow_stream on a flow without a stream is a safe no-op") {
    roqr::quic::Client client;
    client.reset_flow_stream(99);  // must not crash before connect
}

TEST_CASE("on_closed fires with NO_ERROR on clean close") {
    roqr::relayd::Server server;
    roqr::relayd::ServerOptions so;
    so.port = 45566;
    so.cert_file = kCertDir + "/cert.pem";
    so.key_file = kCertDir + "/key.pem";
    REQUIRE(server.start(so));

    std::atomic<uint64_t> code{999};
    std::atomic<bool> fired{false};
    roqr::quic::Client client;
    client.on_closed([&](uint64_t c) { code = c; fired = true; });
    REQUIRE(client.connect("127.0.0.1", 45566));
    REQUIRE(client.wait_connected(5s));
    client.close(roqr::ErrorCode::NoError);
    REQUIRE(client.wait_closed(5s));
    CHECK(fired.load());
    CHECK(code.load() == 0);
    server.stop();
}
