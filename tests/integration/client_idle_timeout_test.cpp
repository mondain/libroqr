#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

#include "roqr/quic/client.hpp"
#include "roqr/relayd/server.hpp"

using namespace std::chrono_literals;

namespace {
const std::string kCertDir = ROQR_TEST_CERT_DIR;
}  // namespace

TEST_CASE("bounded idle timeout detects a silently dropped server") {
    roqr::relayd::Server relay;
    roqr::relayd::ServerOptions ro;
    ro.port = 45590;
    ro.cert_file = kCertDir + "/cert.pem";
    ro.key_file = kCertDir + "/key.pem";
    REQUIRE(relay.start(ro));

    std::mutex mutex;
    std::condition_variable cv;
    bool closed = false;

    roqr::quic::Client client;
    client.on_closed([&](uint64_t /*app_error_code*/) {
        std::lock_guard<std::mutex> lock(mutex);
        closed = true;
        cv.notify_all();
    });

    roqr::quic::ClientOptions co;
    co.idle_timeout = 1500ms;
    REQUIRE(client.connect("127.0.0.1", ro.port, co));
    REQUIRE(client.wait_connected(5s));

    // The relay tears down its network thread without sending
    // CONNECTION_CLOSE: from the client's point of view the server just
    // goes silent, so only the bounded idle timeout notices the drop.
    relay.stop();

    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(cv.wait_for(lock, 3s, [&] { return closed; }));
}
