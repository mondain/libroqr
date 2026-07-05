#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "roqr/gateway/rtmp_commands.hpp"
#include "roqr/relayd/server.hpp"
#include "roqr/rtmp/chunk_reader.hpp"
#include "roqr/rtmp/chunk_writer.hpp"
#include "roqr/rtmp/handshake.hpp"

extern "C" {
#include "roqr/roqr_rtmp.h"
}

using namespace std::chrono_literals;

namespace {
const std::string kCertDir = ROQR_TEST_CERT_DIR;

bool send_all(int fd, const std::vector<uint8_t>& d) {
    size_t off = 0;
    while (off < d.size()) {
        ssize_t n = ::send(fd, d.data() + off, d.size() - off, 0);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

int connect_tcp(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool do_handshake_client(int fd, roqr::rtmp::HandshakeInitiator& hs) {
    if (!send_all(fd, hs.start())) return false;
    uint8_t buf[4096];
    std::vector<uint8_t> c2;
    while (!hs.done()) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        if (!hs.feed(std::span<const uint8_t>(buf, static_cast<size_t>(n)),
                     c2)) {
            return false;
        }
        if (!c2.empty()) {
            send_all(fd, c2);
            c2.clear();
        }
    }
    return true;
}
}  // namespace

TEST_CASE("ffi ingest and egress carry video end to end") {
    roqr::relayd::Server relay;
    roqr::relayd::ServerOptions ro;
    ro.port = 45602;
    ro.cert_file = kCertDir + "/cert.pem";
    ro.key_file = kCertDir + "/key.pem";
    ro.mode = roqr::relayd::Mode::Media;
    REQUIRE(relay.start(ro));

    roqr_egress* eg = roqr_egress_create();
    REQUIRE(roqr_egress_start(eg, 45604, "127.0.0.1", 45602, "cam", 1) ==
            ROQR_OK);
    REQUIRE(roqr_egress_wait_playing(eg, 5000) == 1);

    roqr_ingest* in = roqr_ingest_create();
    REQUIRE(roqr_ingest_start(in, 45603, "127.0.0.1", 45602, 1) == ROQR_OK);

    // RTMP publisher into ingest.
    roqr::rtmp::HandshakeInitiator phs;
    roqr::rtmp::ChunkWriter pw;
    int pub = connect_tcp(45603);
    REQUIRE(pub >= 0);
    REQUIRE(do_handshake_client(pub, phs));
    auto send_cmd = [&](const roqr::rtmp::RtmpMessage& m) {
        std::vector<uint8_t> wire;
        pw.write(m, wire);
        send_all(pub, wire);
    };
    send_cmd(roqr::gateway::build_connect(1, "live", "rtmp://h/live"));
    send_cmd(roqr::gateway::build_create_stream(2));
    send_cmd(roqr::gateway::build_publish(3, "cam"));
    REQUIRE(roqr_ingest_wait_publishing(in, 5000) == 1);

    // RTMP player out of egress.
    roqr::rtmp::HandshakeInitiator lhs;
    roqr::rtmp::ChunkWriter lw;
    roqr::rtmp::ChunkReader lr;
    int play = connect_tcp(45604);
    REQUIRE(play >= 0);
    REQUIRE(do_handshake_client(play, lhs));
    auto play_cmd = [&](const roqr::rtmp::RtmpMessage& m) {
        std::vector<uint8_t> wire;
        lw.write(m, wire);
        send_all(play, wire);
    };
    play_cmd(roqr::gateway::build_connect(1, "live", "rtmp://h/live"));
    play_cmd(roqr::gateway::build_create_stream(2));
    play_cmd(roqr::gateway::build_play(3, "cam"));
    std::this_thread::sleep_for(200ms);

    // Publish a seq header + keyframe.
    auto vid = [&](uint32_t ts, std::vector<uint8_t> p) {
        roqr::rtmp::RtmpMessage m;
        m.type = 9;
        m.timestamp = ts;
        m.message_stream_id = 1;
        m.chunk_stream_id = 6;
        m.payload = std::move(p);
        std::vector<uint8_t> wire;
        pw.write(m, wire);
        send_all(pub, wire);
    };
    vid(0, {0x17, 0x00, 0x11});
    vid(40, {0x17, 0x01, 0x22});

    // The player must receive a video message.
    bool got_video = false;
    uint8_t buf[4096];
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!got_video && std::chrono::steady_clock::now() < deadline) {
        while (auto m = lr.next()) {
            if (m->type == 9) got_video = true;
        }
        if (got_video) break;
        ssize_t n = ::recv(play, buf, sizeof(buf), 0);
        if (n <= 0) break;
        lr.feed(std::span<const uint8_t>(buf, static_cast<size_t>(n)));
    }
    CHECK(got_video);

    ::close(pub);
    ::close(play);
    roqr_ingest_stop(in);
    roqr_ingest_destroy(in);
    roqr_egress_stop(eg);
    roqr_egress_destroy(eg);
    relay.stop();
}

TEST_CASE("ffi gateway create/destroy and null-arg safety") {
    roqr_ingest* in = roqr_ingest_create();
    REQUIRE(in != nullptr);
    roqr_ingest_stop(in);        // stop before start is safe
    roqr_ingest_destroy(in);
    roqr_ingest_destroy(nullptr);

    roqr_egress* eg = roqr_egress_create();
    REQUIRE(eg != nullptr);
    CHECK(roqr_egress_start(nullptr, 1, "h", 2, "s", 1) ==
          ROQR_ERR_INVALID_ARG);
    roqr_egress_destroy(eg);
}
