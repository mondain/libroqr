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

roqr::Frame media(uint64_t ts, size_t payload_size) {
    roqr::Frame f;
    f.flow_id = 0;
    f.timestamp = ts;
    f.message_type = 9;
    f.message_stream_id = 1;
    f.chunk_stream_id = 4;
    f.payload.assign(payload_size, 0xEE);
    return f;
}
}  // namespace

TEST_CASE("small media frame round-trips as a datagram") {
    roqr::relayd::Server server;
    roqr::relayd::ServerOptions so;
    so.port = 45555;
    so.cert_file = kCertDir + "/cert.pem";
    so.key_file = kCertDir + "/key.pem";
    REQUIRE(server.start(so));

    Collector got;
    roqr::quic::Client client;
    client.on_message([&](const roqr::Frame& f) { got.add(f); });
    REQUIRE(client.connect("127.0.0.1", 45555));
    REQUIRE(client.wait_connected(5s));
    CHECK(client.datagrams_negotiated());

    const auto f = media(300, 100);
    REQUIRE(client.send(f, roqr::quic::DeliveryMode::Datagram));
    REQUIRE(got.wait_count(1, 5s));
    {
        std::lock_guard lock(got.mutex);
        CHECK(got.frames[0] == f);
    }
    CHECK(client.datagrams_sent() == 1);
    CHECK(client.datagrams_dropped() == 0);
    client.close();
    client.wait_closed(5s);
    server.stop();
}

TEST_CASE("oversized datagram falls back to stream and still arrives") {
    roqr::relayd::Server server;
    roqr::relayd::ServerOptions so;
    so.port = 45556;
    so.cert_file = kCertDir + "/cert.pem";
    so.key_file = kCertDir + "/key.pem";
    REQUIRE(server.start(so));

    Collector got;
    roqr::quic::Client client;
    client.on_message([&](const roqr::Frame& f) { got.add(f); });
    REQUIRE(client.connect("127.0.0.1", 45556));
    REQUIRE(client.wait_connected(5s));

    // Far larger than any datagram: must arrive via the stream fallback.
    const auto f = media(400, 10000);
    REQUIRE(client.send(f, roqr::quic::DeliveryMode::Datagram));
    REQUIRE(got.wait_count(1, 5s));
    {
        std::lock_guard lock(got.mutex);
        CHECK(got.frames[0] == f);
    }
    CHECK(client.datagrams_sent() == 0);
    CHECK(client.datagrams_dropped() == 0);
    client.close();
    client.wait_closed(5s);
    server.stop();
}
