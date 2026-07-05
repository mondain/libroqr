#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "roqr/gateway/bridge.hpp"
#include "roqr/gateway/ingest.hpp"
#include "roqr/gateway/rtmp_commands.hpp"
#include "roqr/quic/client.hpp"
#include "roqr/relayd/server.hpp"
#include "roqr/rtmp/chunk_writer.hpp"
#include "roqr/rtmp/handshake.hpp"

using namespace std::chrono_literals;
using roqr::gateway::to_frame;

namespace {
const std::string kCertDir = ROQR_TEST_CERT_DIR;

// Minimal RTMP publisher client (drives ingest's RTMP listener).
struct RtmpPublisher {
    int fd = -1;
    roqr::rtmp::HandshakeInitiator hs;
    roqr::rtmp::ChunkWriter writer;

    bool connect_and_publish(uint16_t port, const std::string& name) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)))
            return false;
        if (!send_all(hs.start())) return false;
        uint8_t buf[4096];
        std::vector<uint8_t> c2;
        while (!hs.done()) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            if (!hs.feed(std::span<const uint8_t>(buf, size_t(n)), c2))
                return false;
            if (!c2.empty()) { send_all(c2); c2.clear(); }
        }
        send_cmd(roqr::gateway::build_connect(1, "live", "rtmp://h/live"));
        send_cmd(roqr::gateway::build_create_stream(2));
        send_cmd(roqr::gateway::build_publish(3, name));
        return true;
    }
    void send_cmd(const roqr::rtmp::RtmpMessage& m) {
        std::vector<uint8_t> wire;
        writer.write(m, wire);
        send_all(wire);
    }
    void send_video(uint32_t ts, std::vector<uint8_t> payload) {
        roqr::rtmp::RtmpMessage m;
        m.type = 9;
        m.timestamp = ts;
        m.message_stream_id = 1;
        m.chunk_stream_id = 6;
        m.payload = std::move(payload);
        std::vector<uint8_t> wire;
        writer.write(m, wire);
        send_all(wire);
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
    ~RtmpPublisher() { if (fd >= 0) ::close(fd); }
};

struct Collector {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<roqr::Frame> frames;
    void add(const roqr::Frame& f) {
        std::lock_guard lock(mutex);
        frames.push_back(f);
        cv.notify_all();
    }
    bool wait_video(std::chrono::milliseconds t) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, t, [&] {
            for (const auto& f : frames)
                if (f.message_type == 9) return true;
            return false;
        });
    }
};
}  // namespace

TEST_CASE("ingest bridges an RTMP publisher onto a RoQR relay") {
    roqr::relayd::Server relay;
    roqr::relayd::ServerOptions ro;
    ro.port = 45582;
    ro.cert_file = kCertDir + "/cert.pem";
    ro.key_file = kCertDir + "/key.pem";
    ro.mode = roqr::relayd::Mode::Media;
    REQUIRE(relay.start(ro));

    // Subscriber plays "cam" from the relay.
    Collector got;
    roqr::quic::Client sub;
    sub.on_message([&](const roqr::Frame& f) { got.add(f); });
    REQUIRE(sub.connect("127.0.0.1", 45582));
    REQUIRE(sub.wait_connected(5s));
    sub.send(to_frame(roqr::gateway::build_connect(1, "live", "rtmp://h"), 0),
             roqr::quic::DeliveryMode::Stream);
    sub.send(to_frame(roqr::gateway::build_create_stream(2), 0),
             roqr::quic::DeliveryMode::Stream);
    sub.send(to_frame(roqr::gateway::build_play(3, "cam"), 0),
             roqr::quic::DeliveryMode::Stream);

    // Ingest in front of the relay.
    roqr::gateway::IngestGateway ingest;
    roqr::gateway::IngestOptions io;
    io.rtmp_port = 45583;
    io.roqr_host = "127.0.0.1";
    io.roqr_port = 45582;
    REQUIRE(ingest.start(io));

    RtmpPublisher pub;
    REQUIRE(pub.connect_and_publish(45583, "cam"));
    REQUIRE(ingest.wait_publishing(5s));
    pub.send_video(0, {0x17, 0x00, 0x11});   // seq header
    pub.send_video(40, {0x17, 0x01, 0x22});  // keyframe

    REQUIRE(got.wait_video(5s));
    ingest.stop();
    sub.close();
    sub.wait_closed(5s);
    relay.stop();
}
