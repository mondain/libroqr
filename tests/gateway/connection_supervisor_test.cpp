#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "roqr/frame.hpp"
#include "roqr/gateway/connection_supervisor.hpp"
#include "roqr/quic/client.hpp"
#include "roqr/relayd/server.hpp"

using namespace std::chrono_literals;
using roqr::gateway::ConnectionSupervisor;
using roqr::gateway::ReconnectPolicy;

namespace {
const std::string kCertDir = ROQR_TEST_CERT_DIR;

roqr::relayd::ServerOptions echo_opts(uint16_t port) {
    roqr::relayd::ServerOptions o;
    o.port = port;
    o.cert_file = kCertDir + "/cert.pem";
    o.key_file = kCertDir + "/key.pem";
    o.mode = roqr::relayd::Mode::Echo;
    return o;
}

roqr::Frame marker_frame() {
    roqr::Frame f;
    f.flow_id = 0;
    f.message_type = 20;  // arbitrary; Echo mode doesn't interpret it
    f.payload = {0x2a};
    return f;
}

// Polls pred every `interval` until it returns true or `timeout` elapses.
// Returns pred()'s final value so callers get a real answer even on timeout.
template <typename Pred>
bool poll_until(Pred pred, std::chrono::milliseconds timeout,
                std::chrono::milliseconds interval = 20ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (pred()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return pred();
        std::this_thread::sleep_for(interval);
    }
}

struct Collector {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<roqr::Frame> frames;

    void add(const roqr::Frame& f) {
        std::lock_guard<std::mutex> lock(mutex);
        frames.push_back(f);
        cv.notify_all();
    }
    bool wait_count_at_least(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout,
                           [&] { return frames.size() >= n; });
    }
};

roqr::quic::ClientOptions short_client_opts() {
    roqr::quic::ClientOptions co;
    co.idle_timeout = 1500ms;
    return co;
}
}  // namespace

TEST_CASE("ConnectionSupervisor reconnects after a drop") {
    const uint16_t port = 45700;
    roqr::relayd::Server relay;
    REQUIRE(relay.start(echo_opts(port)));

    Collector got;
    ReconnectPolicy policy;
    policy.connect_timeout = 800ms;
    policy.initial_backoff = 100ms;
    policy.max_backoff = 300ms;
    policy.max_attempts = 0;  // don't give up in this test

    ConnectionSupervisor sup(
        "127.0.0.1", port, short_client_opts(), policy,
        [](roqr::quic::Client& c) {
            c.send(marker_frame(), roqr::quic::DeliveryMode::Stream);
        },
        [&](const roqr::Frame& f) { got.add(f); });

    sup.start();
    REQUIRE(poll_until([&] { return sup.connected(); }, 3s));
    REQUIRE(got.wait_count_at_least(1, 3s));

    relay.stop();
    REQUIRE(poll_until([&] { return !sup.connected(); }, 4s));

    REQUIRE(relay.start(echo_opts(port)));
    REQUIRE(poll_until(
        [&] { return sup.connected() && sup.reconnect_count() >= 1; }, 5s));
    // on_ready ran again on the fresh connection: a second echo arrives.
    REQUIRE(got.wait_count_at_least(2, 3s));

    sup.stop();
    relay.stop();
}

TEST_CASE("ConnectionSupervisor reconnects after a drop without backoff delay") {
    // Uses a LARGE backoff so this test can actually discriminate "immediate
    // retry after a drop" from "retry after (even a short) backoff": if the
    // post-drop retry were wrongly routed through handle_failed_attempt's
    // backoff path, reconnection would take >3s and this test would time
    // out well before that.
    const uint16_t port = 45705;
    roqr::relayd::Server relay;
    REQUIRE(relay.start(echo_opts(port)));

    ReconnectPolicy policy;
    policy.connect_timeout = 800ms;
    policy.initial_backoff = 3000ms;
    policy.max_backoff = 3000ms;
    policy.max_attempts = 0;  // don't give up in this test

    ConnectionSupervisor sup(
        "127.0.0.1", port, short_client_opts(), policy,
        [](roqr::quic::Client&) {}, [](const roqr::Frame&) {});

    sup.start();
    REQUIRE(poll_until([&] { return sup.connected(); }, 3s));

    relay.stop();
    REQUIRE(poll_until([&] { return !sup.connected(); }, 4s));
    REQUIRE(relay.start(echo_opts(port)));

    // Measure from the moment the drop is observed (client back down), so
    // the assertion is purely about the reconnect-after-drop latency, not
    // drop-detection latency (which depends on idle_timeout).
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(poll_until(
        [&] { return sup.connected() && sup.reconnect_count() >= 1; }, 1500ms));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(elapsed < 1500ms);

    sup.stop();
    relay.stop();
}

TEST_CASE("ConnectionSupervisor gives up after max_attempts consecutive failures") {
    const uint16_t port = 45701;  // nothing listening

    ReconnectPolicy policy;
    policy.connect_timeout = 500ms;
    policy.initial_backoff = 100ms;
    policy.max_backoff = 300ms;
    policy.max_attempts = 3;

    ConnectionSupervisor sup(
        "127.0.0.1", port, short_client_opts(), policy,
        [](roqr::quic::Client&) {}, [](const roqr::Frame&) {});

    sup.start();
    REQUIRE(poll_until([&] { return sup.failed(); }, 6s));
    CHECK_FALSE(sup.connected());
    CHECK(sup.reconnect_count() == 0);
    sup.stop();
}

TEST_CASE("ConnectionSupervisor stop() during backoff is prompt") {
    const uint16_t port = 45702;  // nothing listening

    ReconnectPolicy policy;
    policy.connect_timeout = 100ms;  // fail fast so backoff starts quickly
    policy.initial_backoff = 3000ms;
    policy.max_backoff = 3000ms;
    policy.max_attempts = 0;  // infinite: never gives up on its own

    ConnectionSupervisor sup(
        "127.0.0.1", port, short_client_opts(), policy,
        [](roqr::quic::Client&) {}, [](const roqr::Frame&) {});

    sup.start();
    std::this_thread::sleep_for(300ms);  // mid-backoff (100ms fail + backoff)

    const auto t0 = std::chrono::steady_clock::now();
    sup.stop();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(elapsed < 1s);
}

TEST_CASE("ConnectionSupervisor stop() during an in-flight connect is prompt") {
    // 192.0.2.1 is RFC 5737 TEST-NET-1: guaranteed unassigned/black-holed, so
    // the handshake never completes and wait_connected() genuinely blocks
    // (as opposed to failing fast). A generous connect_timeout and backoff
    // make sure a passing test is due to stop() interrupting the block, not
    // to the timeout/backoff path racing it.
    //
    // This exercises the practically-hittable path: the connect attempt is
    // published to `client` and wait_connected() is blocked when stop() is
    // called, so stop() must close() the published client to unblock it
    // promptly. The narrower race -- stop() landing in the small window
    // before publish, while client is still nullptr -- is closed by
    // construction in connection_supervisor.cpp (the publish block
    // re-checks `stopping` under the lock and calls close() immediately if
    // set), so it isn't separately re-tested here.
    ReconnectPolicy policy;
    policy.connect_timeout = 4000ms;
    policy.initial_backoff = 4000ms;
    policy.max_backoff = 4000ms;
    policy.max_attempts = 0;

    ConnectionSupervisor sup(
        "192.0.2.1", 45704, short_client_opts(), policy,
        [](roqr::quic::Client&) {}, [](const roqr::Frame&) {});

    sup.start();
    std::this_thread::sleep_for(100ms);  // let the connect attempt get in
                                         // flight (client published, blocked
                                         // in wait_connected)

    const auto t0 = std::chrono::steady_clock::now();
    sup.stop();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(elapsed < 1s);
}

TEST_CASE(
    "ConnectionSupervisor reconnects when the server drops immediately "
    "after connect (no lost-wakeup stall)") {
    // Regression test for a lost-wakeup race: on_closed sets drop_signaled
    // on the network thread, but the success path used to reset
    // drop_signaled=false right after the connection came up. If the
    // connection died between coming up and that reset -- or during
    // wait_connected itself (wait_connected returns up==true even then,
    // since `connected` is never cleared on close) -- the on_closed signal
    // was erased and the subsequent cv.wait() blocked forever: no
    // reconnect, no failed(), a silent permanent stall.
    //
    // relayd's close_after_ready option makes the server close every
    // connection the instant it completes the handshake, on the server's
    // own thread -- as close in time to the client's `ready` event as this
    // test harness can get without reaching into picoquic directly. Each
    // connect/serve/teardown cycle is fast (sub-handshake-RTT over
    // loopback), so this test runs many such cycles per second, turning
    // what is normally a nanosecond-wide race into one that should be hit
    // repeatedly within the test's deadline: with the bug present, the
    // supervisor is very likely to wedge on one of the early cycles and
    // reconnect_count() stops advancing forever; with the fix, every cycle
    // completes and reconnect_count() climbs steadily the whole time.
    const uint16_t port = 45706;
    roqr::relayd::ServerOptions ro = echo_opts(port);
    ro.close_after_ready = true;
    roqr::relayd::Server relay;
    REQUIRE(relay.start(ro));

    ReconnectPolicy policy;
    policy.connect_timeout = 800ms;
    policy.initial_backoff = 100ms;
    policy.max_backoff = 300ms;
    policy.max_attempts = 0;  // don't give up: a flapping server is expected

    ConnectionSupervisor sup(
        "127.0.0.1", port, short_client_opts(), policy,
        [](roqr::quic::Client&) {}, [](const roqr::Frame&) {});

    sup.start();

    // If the bug is present, reconnect_count() will stall at whatever value
    // it reached before the wedge; poll_until below will time out on that
    // stalled value. If fixed, the loop keeps advancing well past 5 within
    // the deadline.
    REQUIRE(poll_until([&] { return sup.reconnect_count() >= 5; }, 5s, 10ms));
    CHECK_FALSE(sup.failed());  // never a connect failure, just rapid drops

    sup.stop();
    relay.stop();
}

TEST_CASE("ConnectionSupervisor stop() before start and double stop are no-ops") {
    ReconnectPolicy policy;
    ConnectionSupervisor sup(
        "127.0.0.1", 45703, short_client_opts(), policy,
        [](roqr::quic::Client&) {}, [](const roqr::Frame&) {});

    sup.stop();  // never started
    sup.stop();  // still never started

    sup.start();
    sup.stop();
    sup.stop();  // double stop after a real start/stop
    SUCCEED("no crash or hang");
}
