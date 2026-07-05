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

#include "roqr/rtmp/amf0.hpp"
#include "roqr/rtmp/chunk_reader.hpp"
#include "roqr/rtmp/chunk_writer.hpp"
#include "roqr/rtmp/handshake.hpp"
#include "roqr/rtmp/server_session.hpp"

using namespace roqr::rtmp;
using namespace std::chrono_literals;

namespace {

// Minimal blocking RTMP client for driving the server under test.
struct TestClient {
    int fd = -1;
    HandshakeInitiator hs;
    ChunkReader reader;
    ChunkWriter writer;

    bool connect_tcp(uint16_t port) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return ::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                         sizeof(addr)) == 0;
    }

    bool send_all(const std::vector<uint8_t>& data) {
        size_t off = 0;
        while (off < data.size()) {
            const ssize_t n =
                ::send(fd, data.data() + off, data.size() - off, 0);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    bool handshake() {
        if (!send_all(hs.start())) return false;
        uint8_t buf[4096];
        std::vector<uint8_t> c2;
        while (!hs.done()) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            if (!hs.feed(std::span<const uint8_t>(buf, static_cast<size_t>(n)),
                         c2)) {
                return false;
            }
            if (!c2.empty()) {
                if (!send_all(c2)) return false;
                c2.clear();
            }
        }
        return true;
    }

    bool send_command(std::vector<Amf0Value> values, uint32_t msid = 0) {
        RtmpMessage m;
        m.chunk_stream_id = 3;
        m.type = kTypeCommandAmf0;
        m.message_stream_id = msid;
        for (const auto& v : values) amf0_encode(v, m.payload);
        std::vector<uint8_t> wire;
        if (!writer.write(m, wire)) return false;
        return send_all(wire);
    }

    // Reads until a command with the given name arrives (applies Set
    // Chunk Size etc. via the reader). Bounded by attempts.
    std::optional<std::vector<Amf0Value>> await_command(
        const std::string& name) {
        uint8_t buf[4096];
        for (int i = 0; i < 200; ++i) {
            while (auto m = reader.next()) {
                if (m->type != kTypeCommandAmf0) continue;
                auto values = amf0_decode_all(m->payload);
                if (values && !values->empty() &&
                    (*values)[0].type() == Amf0Value::Type::String &&
                    (*values)[0].as_string() == name) {
                    return values;
                }
            }
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return std::nullopt;
            reader.feed(std::span<const uint8_t>(buf, static_cast<size_t>(n)));
        }
        return std::nullopt;
    }

    ~TestClient() {
        if (fd >= 0) ::close(fd);
    }
};

struct Events {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<RtmpMessage> messages;
    std::string app, stream;
    bool got_publish = false;

    SessionEvents make() {
        SessionEvents e;
        e.on_connect = [this](ServerSession&, const std::string& a) {
            std::lock_guard lock(mutex);
            app = a;
            cv.notify_all();
        };
        e.on_stream = [this](ServerSession&, const std::string& s, bool pub) {
            std::lock_guard lock(mutex);
            stream = s;
            got_publish = pub;
            cv.notify_all();
        };
        e.on_message = [this](ServerSession&, const RtmpMessage& m) {
            std::lock_guard lock(mutex);
            messages.push_back(m);
            cv.notify_all();
        };
        return e;
    }

    bool wait_messages(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout,
                           [&] { return messages.size() >= n; });
    }
};

}  // namespace

TEST_CASE("full publish flow over loopback TCP") {
    Events events;
    Listener listener;
    REQUIRE(listener.start(45570,
                           [&](ServerSession&) { return events.make(); }));

    TestClient client;
    REQUIRE(client.connect_tcp(45570));
    REQUIRE(client.handshake());

    Amf0Value cmd_obj = Amf0Value::object();
    cmd_obj.set("app", Amf0Value::string("live"))
        .set("tcUrl", Amf0Value::string("rtmp://127.0.0.1/live"));
    REQUIRE(client.send_command(
        {Amf0Value::string("connect"), Amf0Value::number(1), cmd_obj}));
    auto result = client.await_command("_result");
    REQUIRE(result.has_value());

    REQUIRE(client.send_command({Amf0Value::string("createStream"),
                                 Amf0Value::number(2), Amf0Value::null()}));
    auto cs = client.await_command("_result");
    REQUIRE(cs.has_value());
    REQUIRE(cs->size() >= 4);
    CHECK((*cs)[3].as_number() == 1.0);

    REQUIRE(client.send_command(
        {Amf0Value::string("publish"), Amf0Value::number(3),
         Amf0Value::null(), Amf0Value::string("mystream"),
         Amf0Value::string("live")},
        1));
    auto status = client.await_command("onStatus");
    REQUIRE(status.has_value());

    // Send an AVC sequence header and two video frames.
    RtmpMessage video;
    video.chunk_stream_id = 4;
    video.type = kTypeVideo;
    video.message_stream_id = 1;
    video.timestamp = 0;
    video.payload = {0x17, 0x00, 0x00, 0x00, 0x00};
    std::vector<uint8_t> wire;
    REQUIRE(client.writer.write(video, wire));
    video.timestamp = 40;
    video.payload = {0x17, 0x01, 0xAA};
    REQUIRE(client.writer.write(video, wire));
    video.timestamp = 80;
    video.payload = {0x27, 0x01, 0xBB};
    REQUIRE(client.writer.write(video, wire));
    REQUIRE(client.send_all(wire));

    REQUIRE(events.wait_messages(3, 5s));
    {
        std::lock_guard lock(events.mutex);
        CHECK(events.app == "live");
        CHECK(events.stream == "mystream");
        CHECK(events.got_publish);
        CHECK(events.messages[0].payload[1] == 0x00);
        CHECK(events.messages[1].timestamp == 40);
        CHECK(events.messages[2].timestamp == 80);
    }
    listener.stop();
}

TEST_CASE("play flow sends stream begin and play status") {
    Events events;
    Listener listener;
    REQUIRE(listener.start(45571,
                           [&](ServerSession&) { return events.make(); }));

    TestClient client;
    REQUIRE(client.connect_tcp(45571));
    REQUIRE(client.handshake());
    REQUIRE(client.send_command({Amf0Value::string("connect"),
                                 Amf0Value::number(1),
                                 Amf0Value::object()}));
    REQUIRE(client.await_command("_result").has_value());
    REQUIRE(client.send_command({Amf0Value::string("createStream"),
                                 Amf0Value::number(2), Amf0Value::null()}));
    REQUIRE(client.await_command("_result").has_value());
    REQUIRE(client.send_command(
        {Amf0Value::string("play"), Amf0Value::number(0), Amf0Value::null(),
         Amf0Value::string("mystream")},
        1));
    auto status = client.await_command("onStatus");
    REQUIRE(status.has_value());
    REQUIRE(status->size() >= 4);
    const Amf0Value* code = (*status)[3].find("code");
    REQUIRE(code != nullptr);
    // Reset arrives first; Start follows.
    CHECK((code->as_string() == "NetStream.Play.Reset" ||
           code->as_string() == "NetStream.Play.Start"));
    listener.stop();
}

TEST_CASE("garbage handshake closes the session without events") {
    Events events;
    Listener listener;
    REQUIRE(listener.start(45572,
                           [&](ServerSession&) { return events.make(); }));

    TestClient client;
    REQUIRE(client.connect_tcp(45572));
    std::vector<uint8_t> garbage(64, 0x55);
    REQUIRE(client.send_all(garbage));
    // Server must drop us; recv sees EOF within the bound.
    uint8_t buf[64];
    ssize_t n;
    do {
        n = ::recv(client.fd, buf, sizeof(buf), 0);
    } while (n > 0);
    CHECK(n == 0);
    {
        std::lock_guard lock(events.mutex);
        CHECK(events.app.empty());
        CHECK(events.messages.empty());
    }
    listener.stop();
}
