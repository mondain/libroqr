#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <mutex>
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
struct RtmpPlayer {
    int fd = -1;
    roqr::rtmp::HandshakeInitiator hs;
    roqr::rtmp::ChunkReader reader;
    roqr::rtmp::ChunkWriter writer;

    // rcvbuf: 0 = don't set (delivery test's default, kernel autotuning
    // applies); > 0 pins SO_RCVBUF before connect so the advertised TCP
    // window is capped at handshake, making a stalled reader's window-full
    // point deterministic regardless of host tcp_rmem/tcp_wmem tuning.
    bool connect_and_play(uint16_t port, const std::string& name,
                          int rcvbuf = 0) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (rcvbuf > 0)
            ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
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
    bool wait_video(std::chrono::milliseconds t) {
        uint8_t buf[4096];
        auto deadline = std::chrono::steady_clock::now() + t;
        while (std::chrono::steady_clock::now() < deadline) {
            while (auto m = reader.next()) {
                if (m->type == 9) return true;
            }
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            reader.feed(std::span<const uint8_t>(buf, size_t(n)));
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
}  // namespace

TEST_CASE("egress plays a RoQR stream out to an RTMP player") {
    roqr::relayd::Server relay;
    roqr::relayd::ServerOptions ro;
    ro.port = 45584;
    ro.cert_file = kCertDir + "/cert.pem";
    ro.key_file = kCertDir + "/key.pem";
    ro.mode = roqr::relayd::Mode::Media;
    REQUIRE(relay.start(ro));

    // Publisher into the relay.
    roqr::quic::Client pub;
    REQUIRE(pub.connect("127.0.0.1", 45584));
    REQUIRE(pub.wait_connected(5s));
    pub.send(to_frame(roqr::gateway::build_connect(1, "live", "rtmp://h"), 0),
             roqr::quic::DeliveryMode::Stream);
    pub.send(to_frame(roqr::gateway::build_create_stream(2), 0),
             roqr::quic::DeliveryMode::Stream);
    pub.send(to_frame(roqr::gateway::build_publish(3, "cam"), 0),
             roqr::quic::DeliveryMode::Stream);
    std::this_thread::sleep_for(200ms);

    // Egress plays "cam" and serves it to an RTMP player.
    roqr::gateway::EgressGateway egress;
    roqr::gateway::EgressOptions eo;
    eo.rtmp_port = 45585;
    eo.roqr_host = "127.0.0.1";
    eo.roqr_port = 45584;
    eo.stream_name = "cam";
    REQUIRE(egress.start(eo));
    REQUIRE(egress.wait_playing(5s));

    RtmpPlayer player;
    REQUIRE(player.connect_and_play(45585, "cam"));
    std::this_thread::sleep_for(200ms);

    // Publish a seq header + keyframe; the player must receive video.
    pub.send(to_frame(vid(0, {0x17, 0x00, 0x11}), 0),
             roqr::quic::DeliveryMode::Stream);
    pub.send(to_frame(vid(40, {0x17, 0x01, 0x22}), 0),
             roqr::quic::DeliveryMode::Stream);

    REQUIRE(player.wait_video(5s));
    egress.stop();
    pub.close();
    pub.wait_closed(5s);
    relay.stop();
}

TEST_CASE("a stalled player does not wedge the egress QUIC thread") {
    roqr::relayd::Server relay;
    roqr::relayd::ServerOptions ro;
    ro.port = 45586;
    ro.cert_file = kCertDir + "/cert.pem";
    ro.key_file = kCertDir + "/key.pem";
    ro.mode = roqr::relayd::Mode::Media;
    REQUIRE(relay.start(ro));

    roqr::quic::Client pub;
    REQUIRE(pub.connect("127.0.0.1", 45586));
    REQUIRE(pub.wait_connected(5s));
    pub.send(to_frame(roqr::gateway::build_connect(1, "live", "rtmp://h"), 0),
             roqr::quic::DeliveryMode::Stream);
    pub.send(to_frame(roqr::gateway::build_create_stream(2), 0),
             roqr::quic::DeliveryMode::Stream);
    pub.send(to_frame(roqr::gateway::build_publish(3, "cam"), 0),
             roqr::quic::DeliveryMode::Stream);
    std::this_thread::sleep_for(200ms);

    roqr::gateway::EgressGateway egress;
    roqr::gateway::EgressOptions eo;
    eo.rtmp_port = 45587;
    eo.roqr_host = "127.0.0.1";
    eo.roqr_port = 45586;
    eo.stream_name = "cam";
    REQUIRE(egress.start(eo));
    REQUIRE(egress.wait_playing(5s));

    // A player that plays then stops reading (stalls its TCP receive). Pin
    // its receive buffer small (rcvbuf=4096; the kernel doubles the request
    // and enforces a ~2304-byte floor on Linux) so the writer blocks after a
    // few KB regardless of the host's tcp_rmem/tcp_wmem autotuning -- a CI
    // runner with larger maxima would otherwise absorb the flood entirely
    // and the assertion below would spuriously fail.
    RtmpPlayer player;
    REQUIRE(player.connect_and_play(45587, "cam", /*rcvbuf=*/4096));
    std::this_thread::sleep_for(200ms);

    // Seq header + a flood of large keyframe-ish frames. The player never
    // reads, so its (now tiny) TCP window fills and the writer thread
    // blocks; the queue fills and starts dropping — but the QUIC network
    // thread (on_frame) keeps accepting, so this loop completes without
    // hanging. With the window pinned small, 1500 x 4000-byte frames is
    // plenty to force drops deterministically and keeps the test fast.
    pub.send(to_frame(vid(0, {0x17, 0x00, 0x11}), 0),
             roqr::quic::DeliveryMode::Stream);
    for (int i = 1; i <= 1500; ++i) {
        pub.send(to_frame(vid(static_cast<uint32_t>(i * 10),
                              std::vector<uint8_t>(4000, 0x27)),
                          0),
                 roqr::quic::DeliveryMode::Stream);
    }

    // Give the pipeline time to fill and drop. Bounded — must not hang.
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (egress.frames_dropped() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }
    CHECK(egress.frames_dropped() > 0);  // stale media dropped, thread alive

    egress.stop();
    pub.close();
    pub.wait_closed(5s);
    relay.stop();
}
