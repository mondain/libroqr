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
}  // namespace

TEST_CASE("relay forwards frames from publisher to subscriber") {
    roqr::relayd::Server server;
    roqr::relayd::ServerOptions so;
    so.port = 45559;
    so.cert_file = kCertDir + "/cert.pem";
    so.key_file = kCertDir + "/key.pem";
    so.mode = roqr::relayd::Mode::Relay;
    REQUIRE(server.start(so));

    Collector pub_got, sub_got;
    roqr::quic::Client subscriber;
    subscriber.on_message([&](const roqr::Frame& f) { sub_got.add(f); });
    REQUIRE(subscriber.connect("127.0.0.1", 45559));
    REQUIRE(subscriber.wait_connected(5s));

    roqr::quic::Client publisher;
    publisher.on_message([&](const roqr::Frame& f) { pub_got.add(f); });
    REQUIRE(publisher.connect("127.0.0.1", 45559));
    REQUIRE(publisher.wait_connected(5s));

    roqr::Frame f;
    f.flow_id = 0;
    f.timestamp = 42;
    f.message_type = 9;
    f.message_stream_id = 1;
    f.chunk_stream_id = 4;
    f.payload = {0xDE, 0xAD, 0xBE, 0xEF};
    REQUIRE(publisher.send(f, roqr::quic::DeliveryMode::Stream));

    REQUIRE(sub_got.wait_count(1, 5s));
    {
        std::lock_guard lock(sub_got.mutex);
        CHECK(sub_got.frames[0] == f);
    }
    // Relay must not echo back to the sender.
    CHECK_FALSE(pub_got.wait_count(1, 1s));

    publisher.close();
    subscriber.close();
    publisher.wait_closed(5s);
    subscriber.wait_closed(5s);
    server.stop();
}
