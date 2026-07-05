#include "roqr/rtmp/server_session.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "roqr/rtmp/amf0.hpp"
#include "roqr/rtmp/chunk_reader.hpp"
#include "roqr/rtmp/chunk_writer.hpp"
#include "roqr/rtmp/handshake.hpp"

namespace roqr::rtmp {

namespace {

constexpr uint32_t kAckWindow = 2'500'000;
constexpr uint32_t kOutChunkSize = 4096;

bool send_all(int fd, const std::vector<uint8_t>& data) {
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd, data.data() + off, data.size() - off,
                                 MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

RtmpMessage make_command(uint32_t csid, uint32_t msid,
                         const std::vector<Amf0Value>& values) {
    RtmpMessage m;
    m.chunk_stream_id = csid;
    m.type = kTypeCommandAmf0;
    m.message_stream_id = msid;
    for (const auto& v : values) amf0_encode(v, m.payload);
    return m;
}

Amf0Value status_info(const std::string& code, const std::string& desc) {
    Amf0Value info = Amf0Value::object();
    info.set("level", Amf0Value::string("status"))
        .set("code", Amf0Value::string(code))
        .set("description", Amf0Value::string(desc));
    return info;
}

RtmpMessage make_control(uint8_t type, std::vector<uint8_t> payload) {
    RtmpMessage m;
    m.chunk_stream_id = 2;
    m.type = type;
    m.message_stream_id = 0;
    m.payload = std::move(payload);
    return m;
}

std::vector<uint8_t> u32be(uint32_t v) {
    return {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
            static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
}

}  // namespace

struct ServerSession::Impl {
    int fd;
    SessionEvents events;
    HandshakeResponder handshake;
    ChunkReader reader;
    // writer + fd writes are guarded by write_mutex (send() is
    // thread-safe; command replies come from the session thread).
    std::mutex write_mutex;
    ChunkWriter writer;
    std::string app;
    std::string stream_name;
    bool publishing = false;
    uint64_t received = 0;
    uint64_t last_ack = 0;
    std::atomic<bool> closing{false};

    explicit Impl(int fd_in, SessionEvents ev)
        : fd(fd_in), events(std::move(ev)) {}

    bool send_message(const RtmpMessage& m) {
        std::lock_guard lock(write_mutex);
        std::vector<uint8_t> wire;
        if (!writer.write(m, wire)) return false;
        return send_all(fd, wire);
    }

    void send_chunk_size() {
        std::lock_guard lock(write_mutex);
        std::vector<uint8_t> wire;
        writer.set_chunk_size(kOutChunkSize, wire);
        send_all(fd, wire);
    }

    void handle_command(ServerSession& self, const RtmpMessage& m) {
        auto values = amf0_decode_all(m.payload);
        if (!values || values->empty() ||
            (*values)[0].type() != Amf0Value::Type::String) {
            return;  // unparseable command: ignore
        }
        const std::string& name = (*values)[0].as_string();
        const double txn =
            values->size() > 1 &&
                    (*values)[1].type() == Amf0Value::Type::Number
                ? (*values)[1].as_number()
                : 0;

        if (name == "connect") {
            if (values->size() > 2) {
                if (const Amf0Value* a = (*values)[2].find("app")) {
                    if (a->type() == Amf0Value::Type::String) {
                        app = a->as_string();
                    }
                }
            }
            send_message(make_control(kTypeWindowAckSize, u32be(kAckWindow)));
            auto bw = u32be(kAckWindow);
            bw.push_back(2);  // dynamic limit
            send_message(make_control(kTypeSetPeerBandwidth, std::move(bw)));
            send_chunk_size();

            Amf0Value props = Amf0Value::object();
            props.set("fmsVer", Amf0Value::string("FMS/3,0,1,123"))
                .set("capabilities", Amf0Value::number(31));
            Amf0Value info = status_info("NetConnection.Connect.Success",
                                         "Connection succeeded.");
            info.set("objectEncoding", Amf0Value::number(0));
            send_message(make_command(
                3, 0,
                {Amf0Value::string("_result"), Amf0Value::number(txn), props,
                 info}));
            if (events.on_connect) events.on_connect(self, app);
        } else if (name == "createStream") {
            send_message(make_command(
                3, 0,
                {Amf0Value::string("_result"), Amf0Value::number(txn),
                 Amf0Value::null(), Amf0Value::number(1)}));
        } else if (name == "publish") {
            if (values->size() > 3 &&
                (*values)[3].type() == Amf0Value::Type::String) {
                stream_name = (*values)[3].as_string();
            }
            publishing = true;
            send_message(make_command(
                5, 1,
                {Amf0Value::string("onStatus"), Amf0Value::number(0),
                 Amf0Value::null(),
                 status_info("NetStream.Publish.Start",
                             "Publishing " + stream_name)}));
            if (events.on_stream) events.on_stream(self, stream_name, true);
        } else if (name == "play") {
            if (values->size() > 3 &&
                (*values)[3].type() == Amf0Value::Type::String) {
                stream_name = (*values)[3].as_string();
            }
            // User Control Stream Begin (event 0) for stream 1.
            std::vector<uint8_t> begin = {0x00, 0x00};
            const auto sid = u32be(1);
            begin.insert(begin.end(), sid.begin(), sid.end());
            send_message(make_control(kTypeUserControl, std::move(begin)));
            send_message(make_command(
                5, 1,
                {Amf0Value::string("onStatus"), Amf0Value::number(0),
                 Amf0Value::null(),
                 status_info("NetStream.Play.Reset",
                             "Resetting " + stream_name)}));
            send_message(make_command(
                5, 1,
                {Amf0Value::string("onStatus"), Amf0Value::number(0),
                 Amf0Value::null(),
                 status_info("NetStream.Play.Start",
                             "Playing " + stream_name)}));
            if (events.on_stream) events.on_stream(self, stream_name, false);
        } else {
            if (events.on_message) events.on_message(self, m);
        }
    }

    void maybe_ack() {
        if (received - last_ack >= kAckWindow) {
            last_ack = received;
            send_message(make_control(
                kTypeAcknowledgement,
                u32be(static_cast<uint32_t>(received & 0xFFFFFFFF))));
        }
    }

    // Drains any messages already assembled by the reader, dispatching
    // them exactly like the main message loop does.
    void drain_ready(ServerSession& self) {
        while (auto m = reader.next()) {
            if (m->type == kTypeCommandAmf0) {
                handle_command(self, *m);
            } else if (m->type == kTypeSetChunkSize ||
                       m->type == kTypeAbort ||
                       m->type == kTypeAcknowledgement ||
                       m->type == kTypeWindowAckSize ||
                       m->type == kTypeSetPeerBandwidth) {
                // Protocol control: reader already applied what matters.
            } else {
                if (events.on_message) events.on_message(self, *m);
            }
        }
    }
};

ServerSession::ServerSession(int fd, SessionEvents events)
    : impl_(std::make_unique<Impl>(fd, std::move(events))) {}

ServerSession::~ServerSession() {
    if (impl_->fd >= 0) ::close(impl_->fd);
}

void ServerSession::set_events(SessionEvents events) {
    impl_->events = std::move(events);
}

void ServerSession::run() {
    uint8_t buf[8192];
    // Handshake phase.
    while (!impl_->handshake.done()) {
        const ssize_t n = ::recv(impl_->fd, buf, sizeof(buf), 0);
        if (n <= 0 || impl_->closing) {
            if (impl_->events.on_close) impl_->events.on_close(*this);
            return;
        }
        std::vector<uint8_t> out;
        if (!impl_->handshake.feed(
                std::span<const uint8_t>(buf, static_cast<size_t>(n)), out)) {
            ::shutdown(impl_->fd, SHUT_RDWR);
            if (impl_->events.on_close) impl_->events.on_close(*this);
            return;
        }
        if (!out.empty() && !send_all(impl_->fd, out)) {
            if (impl_->events.on_close) impl_->events.on_close(*this);
            return;
        }
    }
    // The handshake may have consumed bytes past C2 in the same TCP
    // segment (e.g. an RTMP chunk pipelined by ffmpeg). Feed those to the
    // chunk reader and count them toward the ack window before the first
    // post-handshake recv, or a pipelined client hangs waiting for a
    // reply that never comes because we never saw its message.
    std::vector<uint8_t> leftover = impl_->handshake.take_leftover();
    if (!leftover.empty()) {
        impl_->received += leftover.size();
        impl_->reader.feed(leftover);
        if (impl_->reader.failed()) {
            ::shutdown(impl_->fd, SHUT_RDWR);
            if (impl_->events.on_close) impl_->events.on_close(*this);
            return;
        }
        impl_->drain_ready(*this);
        impl_->maybe_ack();
    }
    // Message phase.
    for (;;) {
        const ssize_t n = ::recv(impl_->fd, buf, sizeof(buf), 0);
        if (n <= 0 || impl_->closing) break;
        impl_->received += static_cast<uint64_t>(n);
        impl_->reader.feed(
            std::span<const uint8_t>(buf, static_cast<size_t>(n)));
        if (impl_->reader.failed()) break;
        impl_->drain_ready(*this);
        impl_->maybe_ack();
    }
    ::shutdown(impl_->fd, SHUT_RDWR);
    if (impl_->events.on_close) impl_->events.on_close(*this);
}

bool ServerSession::send(const RtmpMessage& msg) {
    return impl_->send_message(msg);
}

void ServerSession::close() {
    impl_->closing = true;
    ::shutdown(impl_->fd, SHUT_RDWR);
}

const std::string& ServerSession::app() const { return impl_->app; }
const std::string& ServerSession::stream_name() const {
    return impl_->stream_name;
}
bool ServerSession::publishing() const { return impl_->publishing; }

struct Listener::Impl {
    int listen_fd = -1;
    std::thread accept_thread;
    std::mutex mutex;
    std::vector<std::unique_ptr<ServerSession>> sessions;
    std::vector<std::thread> session_threads;
    EventsFactory factory;
    std::atomic<bool> running{false};
};

Listener::Listener() : impl_(std::make_unique<Impl>()) {}
Listener::~Listener() { stop(); }

bool Listener::start(uint16_t port, EventsFactory factory) {
    if (impl_->running) return false;
    impl_->factory = std::move(factory);
    impl_->listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (impl_->listen_fd < 0) return false;
    const int one = 1;
    ::setsockopt(impl_->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one,
                 sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(impl_->listen_fd, reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) != 0 ||
        ::listen(impl_->listen_fd, 8) != 0) {
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
        return false;
    }
    impl_->running = true;
    impl_->accept_thread = std::thread([this] {
        while (impl_->running) {
            const int fd = ::accept(impl_->listen_fd, nullptr, nullptr);
            if (fd < 0) break;  // listen socket closed by stop()
            std::lock_guard lock(impl_->mutex);
            auto session = std::make_unique<ServerSession>(fd,
                                                            SessionEvents{});
            ServerSession* raw = session.get();
            // The factory sees the session before its thread starts.
            raw->set_events(impl_->factory(*raw));
            impl_->sessions.push_back(std::move(session));
            impl_->session_threads.emplace_back(
                [raw] { raw->run(); });
        }
    });
    return true;
}

void Listener::stop() {
    if (!impl_->running) return;
    impl_->running = false;
    if (impl_->listen_fd >= 0) {
        ::shutdown(impl_->listen_fd, SHUT_RDWR);
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
    }
    if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
    {
        std::lock_guard lock(impl_->mutex);
        for (auto& s : impl_->sessions) s->close();
    }
    for (auto& t : impl_->session_threads) {
        if (t.joinable()) t.join();
    }
    std::lock_guard lock(impl_->mutex);
    impl_->sessions.clear();
    impl_->session_threads.clear();
}

}  // namespace roqr::rtmp
