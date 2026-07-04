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
};

roqr::Frame video_frame(uint64_t ts, std::vector<uint8_t> payload) {
    roqr::Frame f;
    f.flow_id = 0;
    f.timestamp = ts;
    f.message_type = 9;
    f.message_stream_id = 1;
    f.chunk_stream_id = 4;
    f.payload = std::move(payload);
    return f;
}
}  // namespace

TEST_CASE("stream frames echo back in order") {
    roqr::relayd::Server server;
    roqr::relayd::ServerOptions so;
    so.port = 45554;
    so.cert_file = kCertDir + "/cert.pem";
    so.key_file = kCertDir + "/key.pem";
    REQUIRE(server.start(so));

    Collector got;
    roqr::quic::Client client;
    client.on_message([&](const roqr::Frame& f) { got.add(f); });
    REQUIRE(client.connect("127.0.0.1", 45554));
    REQUIRE(client.wait_connected(5s));

    const auto a = video_frame(100, {0xAA, 0xBB});
    const auto b = video_frame(200, std::vector<uint8_t>(3000, 0xCC));
    REQUIRE(client.send(a, roqr::quic::DeliveryMode::Stream));
    REQUIRE(client.send(b, roqr::quic::DeliveryMode::Stream));

    REQUIRE(got.wait_count(2, 5s));
    {
        std::lock_guard lock(got.mutex);
        CHECK(got.frames[0] == a);
        CHECK(got.frames[1] == b);
    }
    client.close();
    client.wait_closed(5s);
    server.stop();
}

TEST_CASE("send fails before connect") {
    roqr::quic::Client client;
    CHECK_FALSE(client.send(video_frame(1, {0x01}),
                            roqr::quic::DeliveryMode::Stream));
}

TEST_CASE("send fails after close is requested") {
    roqr::relayd::Server server;
    roqr::relayd::ServerOptions so;
    so.port = 45562;
    so.cert_file = kCertDir + "/cert.pem";
    so.key_file = kCertDir + "/key.pem";
    REQUIRE(server.start(so));

    roqr::quic::Client client;
    REQUIRE(client.connect("127.0.0.1", 45562));
    REQUIRE(client.wait_connected(5s));
    client.close();
    CHECK_FALSE(client.send(video_frame(1, {0x01}),
                            roqr::quic::DeliveryMode::Stream));
    client.wait_closed(5s);
    server.stop();
}
