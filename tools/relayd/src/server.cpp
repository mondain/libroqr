#include "roqr/relayd/server.hpp"

#include <sys/stat.h>

#include <map>
#include <mutex>
#include <vector>

#include <picoquic.h>
#include <picoquic_packet_loop.h>
#include <picoquic_utils.h>

#include "roqr/frame.hpp"
#include "roqr/quic/context.hpp"

namespace roqr::relayd {

namespace {
bool file_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
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

    // Per-connection state, touched only on the network thread.
    struct Conn {
        std::map<uint64_t, roqr::FrameDecoder> decoders;  // by stream id
    };
    std::map<picoquic_cnx_t*, Conn> conns;

    static int connection_callback(picoquic_cnx_t* cnx, uint64_t stream_id,
                                   uint8_t* bytes, size_t length,
                                   picoquic_call_back_event_t event,
                                   void* callback_ctx, void* stream_ctx);
    static int loop_callback(picoquic_quic_t* quic,
                             picoquic_packet_loop_cb_enum cb_mode,
                             void* callback_ctx, void* callback_arg);

    void echo_stream_frames(picoquic_cnx_t* cnx, uint64_t stream_id,
                            const uint8_t* bytes, size_t length);
};

void Server::Impl::echo_stream_frames(picoquic_cnx_t* cnx,
                                      uint64_t stream_id,
                                      const uint8_t* bytes, size_t length) {
    auto& decoder = conns[cnx].decoders.try_emplace(stream_id).first->second;
    decoder.feed(std::span<const uint8_t>(bytes, length));
    while (auto frame = decoder.next()) {
        std::vector<uint8_t> wire;
        if (roqr::frame_encode(*frame, wire)) {
            picoquic_add_to_stream(cnx, stream_id, wire.data(), wire.size(),
                                   0);
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
        case picoquic_callback_stream_data:
            impl->echo_stream_frames(cnx, stream_id, bytes, length);
            break;
        case picoquic_callback_stream_fin:
            impl->echo_stream_frames(cnx, stream_id, bytes, length);
            impl->conns[cnx].decoders.erase(stream_id);
            break;
        case picoquic_callback_stream_reset:
        case picoquic_callback_stop_sending:
            impl->conns[cnx].decoders.erase(stream_id);
            break;
        case picoquic_callback_datagram: {
            roqr::Frame frame;
            if (roqr::datagram_decode(std::span<const uint8_t>(bytes, length),
                                      frame) == roqr::DecodeStatus::Ok) {
                std::vector<uint8_t> wire;
                if (roqr::frame_encode(frame, wire)) {
                    picoquic_queue_datagram_frame(cnx, wire.size(),
                                                  wire.data());
                }
            }
            // Malformed datagrams are dropped without closing (draft s12).
            break;
        }
        case picoquic_callback_close:
        case picoquic_callback_application_close:
            impl->conns.erase(cnx);
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
