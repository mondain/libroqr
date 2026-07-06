#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <thread>
#include <vector>

#include "roqr/gateway/bridge.hpp"
#include "roqr/gateway/egress.hpp"
#include "roqr/gateway/rtmp_commands.hpp"
#include "roqr/quic/client.hpp"
#include "roqr/relayd/server.hpp"
#include "roqr/rtmp/chunk_reader.hpp"
#include "roqr/rtmp/chunk_writer.hpp"
#include "roqr/rtmp/handshake.hpp"

using namespace std::chrono_literals;
using roqr::gateway::to_frame;

namespace {
const std::string kCertDir = ROQR_TEST_CERT_DIR;

// Minimal RTMP player (drives egress's RTMP listener, receives media).
// Counts video frames cumulatively (rather than a one-shot "saw a video
// frame" bool) so a second reconnect-phase assertion can require STRICTLY
// MORE video than a recorded baseline -- proving genuinely new media
// arrived after the gateway reconnects, not a stale frame that was already
// sitting in the ChunkReader's parsed-but-undelivered queue from the first
// phase.
struct RtmpPlayer {
    int fd = -1;
    roqr::rtmp::HandshakeInitiator hs;
    roqr::rtmp::ChunkReader reader;
    roqr::rtmp::ChunkWriter writer;
    size_t video_count = 0;

    bool connect_and_play(uint16_t port, const std::string& name) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        // Bound every recv() below so a broken reconnect (no more data ever
        // arrives) fails the test via a timed-out assertion instead of a
        // hung process.
        timeval tv{1, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)))
            return false;
        send_all(hs.start());
        uint8_t buf[4096];
        std::vector<uint8_t> c2;
        while (!hs.done()) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            hs.feed(std::span<const uint8_t>(buf, size_t(n)), c2);
            if (!c2.empty()) { send_all(c2); c2.clear(); }
        }
        send_cmd(roqr::gateway::build_connect(1, "live", "rtmp://h/live"));
        send_cmd(roqr::gateway::build_create_stream(2));
        send_cmd(roqr::gateway::build_play(3, name));
        return true;
    }
    void send_cmd(const roqr::rtmp::RtmpMessage& m) {
        std::vector<uint8_t> wire;
        writer.write(m, wire);
        send_all(wire);
    }
    void drain() {
        while (auto m = reader.next()) {
            if (m->type == 9) ++video_count;
        }
    }
    // Waits until the cumulative video-frame count reaches at least `n`.
    bool wait_video_at_least(size_t n, std::chrono::milliseconds t) {
        drain();
        if (video_count >= n) return true;
        uint8_t buf[4096];
        auto deadline = std::chrono::steady_clock::now() + t;
        while (std::chrono::steady_clock::now() < deadline) {
            ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
            if (r > 0) {
                reader.feed(std::span<const uint8_t>(buf, size_t(r)));
                drain();
                if (video_count >= n) return true;
            } else if (r == 0) {
                return false;  // peer closed
            }
            // r < 0: SO_RCVTIMEO expiry (EAGAIN/EWOULDBLOCK) or EINTR --
            // loop around and re-check the deadline.
        }
        return false;
    }
    bool send_all(const std::vector<uint8_t>& d) {
        size_t off = 0;
        while (off < d.size()) {
            ssize_t n = ::send(fd, d.data() + off, d.size() - off, 0);
            if (n <= 0) return false;
            off += size_t(n);
        }
        return true;
    }
    ~RtmpPlayer() { if (fd >= 0) ::close(fd); }
};

roqr::rtmp::RtmpMessage vid(uint32_t ts, std::vector<uint8_t> p) {
    roqr::rtmp::RtmpMessage m;
    m.type = 9;
    m.timestamp = ts;
    m.message_stream_id = 1;
    m.chunk_stream_id = 6;
    m.payload = std::move(p);
    return m;
}

// Connects a raw publisher Client and sends the connect/createStream/publish
// handshake for `name`. Mirrors the gateway's own on_ready handshake.
void publish_handshake(roqr::quic::Client& c, const std::string& name) {
    c.send(to_frame(roqr::gateway::build_connect(1, "live", "rtmp://h"), 0),
           roqr::quic::DeliveryMode::Stream);
    c.send(to_frame(roqr::gateway::build_create_stream(2), 0),
           roqr::quic::DeliveryMode::Stream);
    c.send(to_frame(roqr::gateway::build_publish(3, name), 0),
           roqr::quic::DeliveryMode::Stream);
}
}  // namespace

TEST_CASE("egress auto-reconnects after the relay drops and serves video again") {
    const uint16_t relay_port = 45610;
    const uint16_t rtmp_port = 45611;

    roqr::relayd::ServerOptions ro;
    ro.port = relay_port;
    ro.cert_file = kCertDir + "/cert.pem";
    ro.key_file = kCertDir + "/key.pem";
    ro.mode = roqr::relayd::Mode::Media;

    roqr::relayd::Server relay;
    REQUIRE(relay.start(ro));

    // Publisher into the relay.
    roqr::quic::Client pub;
    REQUIRE(pub.connect("127.0.0.1", relay_port));
    REQUIRE(pub.wait_connected(5s));
    publish_handshake(pub, "cam");
    std::this_thread::sleep_for(200ms);

    // Egress plays "cam" and serves it to an RTMP player. Short idle_timeout
    // so the drop below is detected quickly; a small connect_timeout/backoff
    // so the reconnect itself is fast once the relay is back.
    roqr::gateway::EgressGateway egress;
    roqr::gateway::EgressOptions eo;
    eo.rtmp_port = rtmp_port;
    eo.roqr_host = "127.0.0.1";
    eo.roqr_port = relay_port;
    eo.stream_name = "cam";
    eo.idle_timeout = 1500ms;
    eo.reconnect.connect_timeout = 2000ms;
    eo.reconnect.initial_backoff = 100ms;
    eo.reconnect.max_backoff = 500ms;
    REQUIRE(egress.start(eo));
    REQUIRE(egress.wait_playing(5s));

    RtmpPlayer player;
    REQUIRE(player.connect_and_play(rtmp_port, "cam"));
    std::this_thread::sleep_for(200ms);

    // Publish a seq header + keyframe; the player must receive video.
    pub.send(to_frame(vid(0, {0x17, 0x00, 0x11}), 0),
             roqr::quic::DeliveryMode::Stream);
    pub.send(to_frame(vid(40, {0x17, 0x01, 0x22}), 0),
             roqr::quic::DeliveryMode::Stream);
    REQUIRE(player.wait_video_at_least(1, 5s));
    const size_t baseline = player.video_count;

    // Simulate a server loss. The relay sends no CONNECTION_CLOSE on
    // shutdown, so egress only learns of the drop via idle_timeout.
    relay.stop();
    REQUIRE(relay.start(ro));  // same port

    // Because a raw publisher Client is also one-shot, the test itself
    // re-establishes the publisher after the restart (not the feature under
    // test, which is the egress gateway's reconnect).
    roqr::quic::Client pub2;
    REQUIRE(pub2.connect("127.0.0.1", relay_port));
    REQUIRE(pub2.wait_connected(5s));
    publish_handshake(pub2, "cam");
    std::this_thread::sleep_for(200ms);
    pub2.send(to_frame(vid(0, {0x17, 0x00, 0x11}), 0),
              roqr::quic::DeliveryMode::Stream);
    pub2.send(to_frame(vid(40, {0x17, 0x01, 0x22}), 0),
              roqr::quic::DeliveryMode::Stream);

    // Generous deadline: idle detection (~1.5s) + reconnect + re-play +
    // relay forward/replay + queue/writer delivery.
    REQUIRE(player.wait_video_at_least(baseline + 1, 10s));

    egress.stop();
    relay.stop();
}
