#include "roqr/relayd/server.hpp"

#include <sys/stat.h>

#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include <picoquic.h>
#include <picoquic_packet_loop.h>
#include <picoquic_utils.h>

#include "roqr/frame.hpp"
#include "roqr/gateway/bridge.hpp"
#include "roqr/gateway/rtmp_commands.hpp"
#include "roqr/quic/context.hpp"
#include "roqr/relayd/media_router.hpp"
#include "roqr/rtmp/amf0.hpp"
#include "roqr/rtmp/classify.hpp"

namespace roqr::relayd {

namespace {
bool file_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

std::string command_stream_name(const roqr::gateway::Command& cmd) {
    for (const auto& a : cmd.args) {
        if (a.type() == roqr::rtmp::Amf0Value::Type::String) {
            return a.as_string();
        }
    }
    return {};
}

// Init frames: onMetaData (18/15) and AVC/AAC/E-RTMP sequence headers.
bool is_init_frame(const roqr::rtmp::RtmpMessage& msg) {
    if (msg.type == 18 || msg.type == 15) return true;
    if (msg.type == 8 || msg.type == 9) {
        const auto info = msg.type == 9
                              ? roqr::rtmp::classify_video(msg.payload)
                              : roqr::rtmp::classify_audio(msg.payload);
        return info.cls == roqr::rtmp::MediaClass::SequenceHeader;
    }
    return false;
}
}  // namespace

struct Server::Impl {
    ServerOptions options;
    std::unique_ptr<roqr::quic::QuicContext> quic;
    picoquic_network_thread_ctx_t* thread_ctx = nullptr;
    int thread_ret = 0;
    // Must outlive the network thread: picoquic_packet_loop_v3 keeps a
    // pointer to this (thread_ctx->param) and both reads and writes it
    // (e.g. send_length_max) for the thread's entire lifetime, so it
    // cannot be a stack-local in start().
    picoquic_packet_loop_param_t loop_param{};
    std::mutex mutex;
    bool running = false;

    // Media mode only: publisher/subscriber registration by stream name.
    MediaRouter router;

    // Per-connection state, touched only on the network thread.
    struct Conn {
        std::map<uint64_t, roqr::FrameDecoder> decoders;  // by stream id
        // Relay only: maps a (source connection, source stream id) pair to
        // the server-initiated bidi stream id opened toward *this*
        // connection to carry it. Stream ids are per-connection and
        // parity-coded by initiator (picoquic rejects a peer stream id the
        // local side hasn't opened), so the source stream id cannot be
        // reused as-is on every other connection; each destination gets its
        // own stream per source stream, preserving per-source ordering.
        std::map<std::pair<picoquic_cnx_t*, uint64_t>, uint64_t>
            relay_streams;
    };
    std::map<picoquic_cnx_t*, Conn> conns;

    static int connection_callback(picoquic_cnx_t* cnx, uint64_t stream_id,
                                   uint8_t* bytes, size_t length,
                                   picoquic_call_back_event_t event,
                                   void* callback_ctx, void* stream_ctx);
    static int loop_callback(picoquic_quic_t* quic,
                             picoquic_packet_loop_cb_enum cb_mode,
                             void* callback_ctx, void* callback_arg);

    void forward_frame(picoquic_cnx_t* from, uint64_t stream_id,
                       const roqr::Frame& frame, bool as_datagram);
    void handle_stream_frames(picoquic_cnx_t* cnx, uint64_t stream_id,
                              const uint8_t* bytes, size_t length);
    // Media mode only: send an RTMP message to cnx as a RoQR frame on
    // flow 0, mirroring the Echo/Relay stream forwarding path.
    void send_rtmp(picoquic_cnx_t* cnx, const roqr::rtmp::RtmpMessage& msg,
                   uint64_t stream_id);
    // Media mode only: parse RTMP commands carried as RoQR frames, handle
    // connect/createStream/publish/play, cache init frames, and route
    // media publisher->subscribers.
    void handle_media_frame(picoquic_cnx_t* cnx, uint64_t stream_id,
                            const roqr::Frame& f, bool as_datagram);
    // Relay only: erase stale relay_streams entries keyed by `source`.
    // If stream_id is set, only the entry for that specific (source,
    // stream_id) pair is erased from every destination's relay_streams
    // (used when a single source stream ends). If stream_id is empty,
    // every entry whose source-cnx component is `source` is erased from
    // every destination (used when the whole source connection closes).
    void purge_relay_streams(picoquic_cnx_t* source,
                             std::optional<uint64_t> stream_id);
};

void Server::Impl::forward_frame(picoquic_cnx_t* from, uint64_t stream_id,
                                 const roqr::Frame& frame, bool as_datagram) {
    std::vector<uint8_t> wire;
    if (!roqr::frame_encode(frame, wire)) return;
    if (options.mode == Mode::Echo) {
        if (as_datagram) {
            picoquic_queue_datagram_frame(from, wire.size(), wire.data());
        } else {
            picoquic_add_to_stream(from, stream_id, wire.data(), wire.size(),
                                   0);
        }
        return;
    }
    // Relay: forward to every other live connection.
    for (auto& [cnx, conn] : conns) {
        if (cnx == from) continue;
        if (as_datagram) {
            picoquic_queue_datagram_frame(cnx, wire.size(), wire.data());
        } else {
            const auto key = std::make_pair(from, stream_id);
            auto it = conn.relay_streams.find(key);
            uint64_t dest_stream_id;
            if (it != conn.relay_streams.end()) {
                dest_stream_id = it->second;
            } else {
                dest_stream_id = picoquic_get_next_local_stream_id(cnx, 0);
                conn.relay_streams.emplace(key, dest_stream_id);
            }
            picoquic_add_to_stream(cnx, dest_stream_id, wire.data(),
                                   wire.size(), 0);
        }
    }
}

void Server::Impl::send_rtmp(picoquic_cnx_t* cnx,
                             const roqr::rtmp::RtmpMessage& msg,
                             uint64_t stream_id) {
    std::vector<uint8_t> wire;
    if (!roqr::frame_encode(roqr::gateway::to_frame(msg, 0), wire)) return;
    picoquic_add_to_stream(cnx, stream_id, wire.data(), wire.size(), 0);
}

void Server::Impl::handle_media_frame(picoquic_cnx_t* cnx, uint64_t stream_id,
                                      const roqr::Frame& f,
                                      bool as_datagram) {
    roqr::rtmp::RtmpMessage msg;
    if (!roqr::gateway::to_rtmp(f, msg)) return;  // malformed width

    if (msg.type == roqr::rtmp::kTypeCommandAmf0) {
        auto cmd = roqr::gateway::parse_command(msg);
        if (!cmd) return;
        if (cmd->name == "connect") {
            roqr::rtmp::Amf0Value props = roqr::rtmp::Amf0Value::object();
            props.set("fmsVer",
                      roqr::rtmp::Amf0Value::string("FMS/3,0,1,123"))
                .set("capabilities", roqr::rtmp::Amf0Value::number(31));
            roqr::rtmp::Amf0Value info = roqr::rtmp::Amf0Value::object();
            info.set("level", roqr::rtmp::Amf0Value::string("status"))
                .set("code", roqr::rtmp::Amf0Value::string(
                                 "NetConnection.Connect.Success"))
                .set("description", roqr::rtmp::Amf0Value::string("ok"))
                .set("objectEncoding", roqr::rtmp::Amf0Value::number(0));
            send_rtmp(cnx,
                      roqr::gateway::build_result_object(
                          cmd->transaction_id, props, info),
                      stream_id);
        } else if (cmd->name == "createStream") {
            send_rtmp(cnx,
                      roqr::gateway::build_result_stream_id(
                          cmd->transaction_id, 1),
                      stream_id);
        } else if (cmd->name == "publish") {
            std::string name = command_stream_name(*cmd);
            router.register_publisher(cnx, name);
            send_rtmp(cnx,
                      roqr::gateway::build_on_status(
                          "NetStream.Publish.Start", "publishing"),
                      stream_id);
        } else if (cmd->name == "play") {
            std::string name = command_stream_name(*cmd);
            router.register_subscriber(cnx, name);
            send_rtmp(cnx,
                      roqr::gateway::build_on_status(
                          "NetStream.Play.Start", "playing"),
                      stream_id);
            // Prime the new subscriber with cached init frames.
            for (auto& bytes : router.init_frames(name)) {
                picoquic_add_to_stream(cnx, stream_id, bytes.data(),
                                       bytes.size(), 0);
            }
        }
        return;
    }

    // Media/data: cache init frames, then route to subscribers.
    if (is_init_frame(msg)) {
        std::vector<uint8_t> bytes;
        if (roqr::frame_encode(f, bytes)) {
            router.cache_init(router.stream_of(cnx), msg.type,
                              std::move(bytes));
        }
    }
    std::vector<uint8_t> wire;
    if (!roqr::frame_encode(f, wire)) return;
    for (void* sub : router.subscribers_for_publisher(cnx)) {
        auto* scnx = static_cast<picoquic_cnx_t*>(sub);
        if (as_datagram) {
            picoquic_queue_datagram_frame(scnx, wire.size(), wire.data());
        } else {
            // Forward on the subscriber's flow-0 stream (id 0).
            picoquic_add_to_stream(scnx, 0, wire.data(), wire.size(), 0);
        }
    }
}

void Server::Impl::purge_relay_streams(picoquic_cnx_t* source,
                                       std::optional<uint64_t> stream_id) {
    for (auto& [cnx, conn] : conns) {
        for (auto it = conn.relay_streams.begin();
             it != conn.relay_streams.end();) {
            const bool same_source = it->first.first == source;
            const bool match = stream_id.has_value()
                                    ? (same_source && it->first.second ==
                                                           *stream_id)
                                    : same_source;
            if (match) {
                it = conn.relay_streams.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void Server::Impl::handle_stream_frames(picoquic_cnx_t* cnx,
                                        uint64_t stream_id,
                                        const uint8_t* bytes, size_t length) {
    auto& decoder = conns[cnx].decoders.try_emplace(stream_id).first->second;
    decoder.feed(std::span<const uint8_t>(bytes, length));
    while (auto frame = decoder.next()) {
        if (options.mode == Mode::Media) {
            handle_media_frame(cnx, stream_id, *frame, /*as_datagram=*/false);
        } else {
            forward_frame(cnx, stream_id, *frame, /*as_datagram=*/false);
        }
    }
}

int Server::Impl::connection_callback(picoquic_cnx_t* cnx,
                                      uint64_t stream_id, uint8_t* bytes,
                                      size_t length,
                                      picoquic_call_back_event_t event,
                                      void* callback_ctx,
                                      void* /*stream_ctx*/) {
    auto* impl = static_cast<Impl*>(callback_ctx);
    switch (event) {
        case picoquic_callback_ready:
            // Track the connection as soon as it is usable, not only once
            // it has sent data, so relay forwarding reaches pure
            // subscribers (connections that never send).
            impl->conns.try_emplace(cnx);
            break;
        case picoquic_callback_stream_data:
            impl->handle_stream_frames(cnx, stream_id, bytes, length);
            break;
        case picoquic_callback_stream_fin:
            impl->handle_stream_frames(cnx, stream_id, bytes, length);
            impl->conns[cnx].decoders.erase(stream_id);
            impl->purge_relay_streams(cnx, stream_id);
            break;
        case picoquic_callback_stream_reset:
        case picoquic_callback_stop_sending: {
            auto it = impl->conns.find(cnx);
            if (it != impl->conns.end()) it->second.decoders.erase(stream_id);
            impl->purge_relay_streams(cnx, stream_id);
            break;
        }
        case picoquic_callback_datagram: {
            roqr::Frame frame;
            if (roqr::datagram_decode(std::span<const uint8_t>(bytes, length),
                                      frame) == roqr::DecodeStatus::Ok) {
                impl->conns.try_emplace(cnx);  // track datagram-only conns
                if (impl->options.mode == Mode::Media) {
                    impl->handle_media_frame(cnx, 0, frame,
                                             /*as_datagram=*/true);
                } else {
                    impl->forward_frame(cnx, 0, frame, /*as_datagram=*/true);
                }
            }
            // Malformed datagrams are dropped without closing (draft s12).
            break;
        }
        case picoquic_callback_close:
        case picoquic_callback_application_close:
            impl->conns.erase(cnx);
            // The dead cnx pointer may be reused by a future connection;
            // scrub every other destination's relay_streams so a later
            // connection reusing this address never inherits stale
            // stream mappings.
            impl->purge_relay_streams(cnx, std::nullopt);
            // Media mode: drop this connection's publisher/subscriber
            // registration so a reused cnx pointer never inherits stale
            // routing.
            impl->router.remove(cnx);
            break;
        default:
            break;
    }
    return 0;
}

int Server::Impl::loop_callback(picoquic_quic_t* /*quic*/,
                                picoquic_packet_loop_cb_enum /*cb_mode*/,
                                void* /*callback_ctx*/,
                                void* /*callback_arg*/) {
    return 0;
}

Server::Server() : impl_(std::make_unique<Impl>()) {}
Server::~Server() { stop(); }

bool Server::start(const ServerOptions& options) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->running) return false;
    if (!file_exists(options.cert_file) || !file_exists(options.key_file)) {
        return false;
    }
    impl_->options = options;
    impl_->quic = roqr::quic::QuicContext::create_server(
        options.alpn, options.cert_file, options.key_file,
        &Impl::connection_callback, impl_.get());
    if (!impl_->quic) return false;

    impl_->loop_param = picoquic_packet_loop_param_t{};
    impl_->loop_param.local_port = options.port;
    impl_->loop_param.local_af = AF_INET;
    impl_->thread_ctx = picoquic_start_network_thread(
        impl_->quic->get(), &impl_->loop_param, &Impl::loop_callback,
        impl_.get(), &impl_->thread_ret);
    if (impl_->thread_ctx == nullptr) {
        impl_->quic.reset();
        return false;
    }
    impl_->running = true;
    return true;
}

void Server::stop() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->running) return;
    picoquic_delete_network_thread(impl_->thread_ctx);
    impl_->thread_ctx = nullptr;
    impl_->quic.reset();
    impl_->conns.clear();
    impl_->running = false;
}

}  // namespace roqr::relayd
