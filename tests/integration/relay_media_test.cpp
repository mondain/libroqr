#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "roqr/gateway/bridge.hpp"
#include "roqr/gateway/rtmp_commands.hpp"
#include "roqr/quic/client.hpp"
#include "roqr/relayd/server.hpp"

using namespace std::chrono_literals;
using roqr::gateway::to_frame;

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
    bool wait_count(size_t n, std::chrono::milliseconds t) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, t, [&] { return frames.size() >= n; });
    }
    size_t count() {
        std::lock_guard lock(mutex);
        return frames.size();
    }
    // Waits for at least n frames of the given RTMP message type. The
    // subscriber also receives its own _result/onStatus command replies
    // (message_type 20) ahead of any forwarded media, so counting all
    // frames races: wait_count(2) can already be true from replies alone,
    // before the publisher's video frames have even been routed. Waiting
    // on the specific media type removes that race.
    bool wait_type_count(uint8_t message_type, size_t n,
                         std::chrono::milliseconds t) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, t, [&] {
            size_t matched = 0;
            for (const auto& f : frames) {
                if (f.message_type == message_type) ++matched;
            }
            return matched >= n;
        });
    }
};

roqr::relayd::ServerOptions media_opts(uint16_t port) {
    roqr::relayd::ServerOptions o;
    o.port = port;
    o.cert_file = kCertDir + "/cert.pem";
    o.key_file = kCertDir + "/key.pem";
    o.mode = roqr::relayd::Mode::Media;
    return o;
}

void send_cmd(roqr::quic::Client& c, const roqr::rtmp::RtmpMessage& m) {
    c.send(to_frame(m, 0), roqr::quic::DeliveryMode::Stream);
}

roqr::rtmp::RtmpMessage media(uint8_t type, uint32_t ts,
                              std::vector<uint8_t> payload) {
    roqr::rtmp::RtmpMessage m;
    m.type = type;
    m.timestamp = ts;
    m.message_stream_id = 1;
    m.chunk_stream_id = type == 8 ? 4 : 6;
    m.payload = std::move(payload);
    return m;
}
}  // namespace

TEST_CASE("media relay forwards a published stream to a subscriber") {
    roqr::relayd::Server server;
    REQUIRE(server.start(media_opts(45580)));

    // Subscriber connects and plays first.
    Collector sub_got;
    roqr::quic::Client sub;
    sub.on_message([&](const roqr::Frame& f) { sub_got.add(f); });
    REQUIRE(sub.connect("127.0.0.1", 45580));
    REQUIRE(sub.wait_connected(5s));
    send_cmd(sub, roqr::gateway::build_connect(1, "live", "rtmp://h/live"));
    send_cmd(sub, roqr::gateway::build_create_stream(2));
    send_cmd(sub, roqr::gateway::build_play(3, "cam"));

    // Publisher connects, publishes, sends a video seq header + a frame.
    roqr::quic::Client pub;
    REQUIRE(pub.connect("127.0.0.1", 45580));
    REQUIRE(pub.wait_connected(5s));
    send_cmd(pub, roqr::gateway::build_connect(1, "live", "rtmp://h/live"));
    send_cmd(pub, roqr::gateway::build_create_stream(2));
    send_cmd(pub, roqr::gateway::build_publish(3, "cam"));
    // Give the publish command time to register before media flows.
    std::this_thread::sleep_for(200ms);
    pub.send(to_frame(media(9, 0, {0x17, 0x00, 0x01}), 0),
             roqr::quic::DeliveryMode::Stream);  // AVC seq header
    pub.send(to_frame(media(9, 40, {0x17, 0x01, 0xAA}), 0),
             roqr::quic::DeliveryMode::Stream);  // keyframe

    // Subscriber should receive the _result/onStatus replies plus the two
    // media frames. Assert at least the two video frames arrive.
    REQUIRE(sub_got.wait_type_count(9, 2, 5s));
    bool saw_seq_header = false, saw_keyframe = false;
    {
        std::lock_guard lock(sub_got.mutex);
        for (const auto& f : sub_got.frames) {
            if (f.message_type == 9 && f.payload.size() >= 2) {
                if (f.payload[1] == 0x00) saw_seq_header = true;
                if (f.payload[1] == 0x01) saw_keyframe = true;
            }
        }
    }
    CHECK(saw_seq_header);
    CHECK(saw_keyframe);

    pub.close();
    sub.close();
    pub.wait_closed(5s);
    sub.wait_closed(5s);
    server.stop();
}

TEST_CASE("a late subscriber is primed with the cached sequence header") {
    roqr::relayd::Server server;
    REQUIRE(server.start(media_opts(45581)));

    roqr::quic::Client pub;
    REQUIRE(pub.connect("127.0.0.1", 45581));
    REQUIRE(pub.wait_connected(5s));
    send_cmd(pub, roqr::gateway::build_connect(1, "live", "rtmp://h/live"));
    send_cmd(pub, roqr::gateway::build_create_stream(2));
    send_cmd(pub, roqr::gateway::build_publish(3, "cam"));
    std::this_thread::sleep_for(200ms);
    pub.send(to_frame(media(9, 0, {0x17, 0x00, 0x99}), 0),
             roqr::quic::DeliveryMode::Stream);  // seq header, cached
    std::this_thread::sleep_for(200ms);

    // Subscriber joins AFTER the seq header was published.
    Collector sub_got;
    roqr::quic::Client sub;
    sub.on_message([&](const roqr::Frame& f) { sub_got.add(f); });
    REQUIRE(sub.connect("127.0.0.1", 45581));
    REQUIRE(sub.wait_connected(5s));
    send_cmd(sub, roqr::gateway::build_connect(1, "live", "rtmp://h/live"));
    send_cmd(sub, roqr::gateway::build_create_stream(2));
    send_cmd(sub, roqr::gateway::build_play(3, "cam"));

    // The cached seq header must be replayed on play, even though the
    // subscriber missed the live one.
    REQUIRE(sub_got.wait_type_count(9, 1, 5s));
    bool primed = false;
    {
        std::lock_guard lock(sub_got.mutex);
        for (const auto& f : sub_got.frames) {
            if (f.message_type == 9 && f.payload.size() >= 3 &&
                f.payload[2] == 0x99) {
                primed = true;
            }
        }
    }
    CHECK(primed);

    pub.close();
    sub.close();
    pub.wait_closed(5s);
    sub.wait_closed(5s);
    server.stop();
}
