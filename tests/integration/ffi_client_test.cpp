#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "roqr/relayd/server.hpp"

extern "C" {
#include "roqr/roqr.h"
}

using namespace std::chrono_literals;

namespace {
const std::string kCertDir = ROQR_TEST_CERT_DIR;

struct Sink {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::vector<uint8_t>> payloads;
    std::atomic<bool> closed{false};
};

void on_message(const roqr_frame* f, void* user) {
    auto* s = static_cast<Sink*>(user);
    std::lock_guard lock(s->mutex);
    s->payloads.emplace_back(f->payload, f->payload + f->payload_len);
    s->cv.notify_all();
}

void on_closed(uint64_t /*code*/, void* user) {
    static_cast<Sink*>(user)->closed = true;
}

roqr_frame make_frame(uint64_t ts, const std::vector<uint8_t>& payload) {
    roqr_frame f{};
    f.flow_id = 0;
    f.timestamp = ts;
    f.message_type = 9;
    f.message_stream_id = 1;
    f.chunk_stream_id = 6;
    f.payload = payload.data();
    f.payload_len = payload.size();
    return f;
}
}  // namespace

TEST_CASE("ffi client echoes a frame through the relay") {
    roqr::relayd::Server server;
    roqr::relayd::ServerOptions so;
    so.port = 45600;
    so.cert_file = kCertDir + "/cert.pem";
    so.key_file = kCertDir + "/key.pem";
    so.mode = roqr::relayd::Mode::Echo;
    REQUIRE(server.start(so));

    Sink sink;
    roqr_client* c = roqr_client_create();
    roqr_client_set_on_message(c, on_message, &sink);
    roqr_client_set_on_closed(c, on_closed, &sink);
    REQUIRE(roqr_client_connect(c, "127.0.0.1", 45600, 1) == ROQR_OK);
    REQUIRE(roqr_client_wait_connected(c, 5000) == 1);

    const std::vector<uint8_t> payload = {0x17, 0x01, 0xAB};
    const roqr_frame f = make_frame(100, payload);
    REQUIRE(roqr_client_send(c, &f, ROQR_DELIVERY_STREAM) == ROQR_OK);

    {
        std::unique_lock lock(sink.mutex);
        REQUIRE(sink.cv.wait_for(lock, 5s,
                                 [&] { return !sink.payloads.empty(); }));
        CHECK(sink.payloads[0] == payload);
    }

    roqr_client_close(c, 0);
    CHECK(roqr_client_wait_closed(c, 5000) == 1);
    roqr_client_destroy(c);
    server.stop();
}

TEST_CASE("ffi send rejects an empty payload and validates args") {
    roqr_client* c = roqr_client_create();
    const roqr_frame empty = make_frame(1, {});
    CHECK(roqr_client_send(c, &empty, ROQR_DELIVERY_STREAM) ==
          ROQR_ERR_INVALID_ARG);
    CHECK(roqr_client_send(nullptr, &empty, ROQR_DELIVERY_STREAM) ==
          ROQR_ERR_INVALID_ARG);
    roqr_client_destroy(c);
}

TEST_CASE("ffi wait_connected times out without a server") {
    roqr_client* c = roqr_client_create();
    REQUIRE(roqr_client_connect(c, "127.0.0.1", 45601, 1) == ROQR_OK);
    CHECK(roqr_client_wait_connected(c, 1500) == 0);
    roqr_client_destroy(c);
}
