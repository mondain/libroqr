#include "roqr/quic/client.hpp"

#include <condition_variable>
#include <map>
#include <mutex>
#include <vector>

#include <picoquic.h>
#include <picoquic_packet_loop.h>
#include <picoquic_utils.h>

#include "roqr/quic/context.hpp"
#include "roqr/quic/outbound_queue.hpp"

namespace roqr::quic {

struct Client::Impl {
    ClientOptions options;
    std::unique_ptr<QuicContext> quic;
    picoquic_cnx_t* cnx = nullptr;  // network thread only after connect
    picoquic_network_thread_ctx_t* thread_ctx = nullptr;
    int thread_ret = 0;
    // Must outlive the network thread: picoquic_packet_loop_v3 keeps a
    // pointer to this (thread_ctx->param) and both reads and writes it
    // (e.g. send_length_max) for the thread's entire lifetime, so it
    // cannot be a stack-local in connect().
    picoquic_packet_loop_param_t loop_param{};

    MessageHandler message_handler;
    ClosedHandler closed_handler;

    std::mutex mutex;
    std::condition_variable cv;
    bool connected = false;
    bool closed = false;
    uint64_t close_code = 0;

    // App -> network thread.
    OutboundQueue outbound;
    bool close_requested = false;
    uint64_t requested_close_code = 0;

    // Network-thread-only state (Tasks 5-7 fill these in).
    roqr::FlowTable flows;
    std::map<uint64_t, roqr::FrameDecoder> decoders;  // by stream id

    void wake() {
        if (thread_ctx != nullptr) {
            picoquic_wake_up_network_thread(thread_ctx);
        }
    }

    void signal_connected() {
        std::lock_guard lock(mutex);
        connected = true;
        cv.notify_all();
    }

    void signal_closed(uint64_t code) {
        {
            std::lock_guard lock(mutex);
            if (closed) return;
            closed = true;
            close_code = code;
            cv.notify_all();
        }
        if (closed_handler) closed_handler(code);
    }

    // Runs on the network thread: apply app-thread requests.
    void service();

    static int connection_callback(picoquic_cnx_t* cnx, uint64_t stream_id,
                                   uint8_t* bytes, size_t length,
                                   picoquic_call_back_event_t event,
                                   void* callback_ctx, void* stream_ctx);
    static int loop_callback(picoquic_quic_t* quic,
                             picoquic_packet_loop_cb_enum cb_mode,
                             void* callback_ctx, void* callback_arg);
};

void Client::Impl::service() {
    bool do_close = false;
    uint64_t code = 0;
    {
        std::lock_guard lock(mutex);
        do_close = close_requested;
        code = requested_close_code;
        close_requested = false;
    }
    if (do_close && cnx != nullptr) {
        picoquic_close(cnx, code);
    }
    // Tasks 5-6 drain `outbound` here.
}

int Client::Impl::connection_callback(picoquic_cnx_t* /*cnx*/,
                                      uint64_t /*stream_id*/,
                                      uint8_t* /*bytes*/, size_t /*length*/,
                                      picoquic_call_back_event_t event,
                                      void* callback_ctx,
                                      void* /*stream_ctx*/) {
    auto* impl = static_cast<Impl*>(callback_ctx);
    switch (event) {
        case picoquic_callback_ready:
            impl->signal_connected();
            break;
        case picoquic_callback_close:
            impl->signal_closed(0);
            break;
        case picoquic_callback_application_close:
            impl->signal_closed(picoquic_get_application_error(impl->cnx));
            break;
        default:
            break;  // stream/datagram events arrive in Tasks 5-6
    }
    return 0;
}

int Client::Impl::loop_callback(picoquic_quic_t* /*quic*/,
                                picoquic_packet_loop_cb_enum cb_mode,
                                void* callback_ctx, void* /*callback_arg*/) {
    auto* impl = static_cast<Impl*>(callback_ctx);
    switch (cb_mode) {
        case picoquic_packet_loop_wake_up:
        case picoquic_packet_loop_after_receive:
        case picoquic_packet_loop_after_send:
            impl->service();
            break;
        default:
            break;
    }
    return 0;
}

Client::Client() : impl_(std::make_unique<Impl>()) {}

Client::~Client() {
    if (impl_->thread_ctx != nullptr) {
        picoquic_delete_network_thread(impl_->thread_ctx);
        impl_->thread_ctx = nullptr;
    }
}

void Client::on_message(MessageHandler h) {
    impl_->message_handler = std::move(h);
}
void Client::on_closed(ClosedHandler h) {
    impl_->closed_handler = std::move(h);
}

bool Client::connect(const std::string& host, uint16_t port,
                     ClientOptions options) {
    impl_->options = std::move(options);
    impl_->flows = roqr::FlowTable(impl_->options.flow_limits);
    impl_->quic = QuicContext::create_client(
        impl_->options.alpn, impl_->options.insecure_skip_verify);
    if (!impl_->quic) return false;

    struct sockaddr_storage addr {};
    int is_name = 0;
    if (picoquic_get_server_address(host.c_str(), port, &addr, &is_name) !=
        0) {
        return false;
    }

    const uint64_t now = picoquic_current_time();
    impl_->cnx = picoquic_create_client_cnx(
        impl_->quic->get(), reinterpret_cast<struct sockaddr*>(&addr), now,
        0, host.c_str(), impl_->options.alpn.c_str(),
        &Impl::connection_callback, impl_.get());
    if (impl_->cnx == nullptr) return false;

    impl_->loop_param = picoquic_packet_loop_param_t{};
    impl_->loop_param.local_af = AF_INET;
    impl_->thread_ctx = picoquic_start_network_thread(
        impl_->quic->get(), &impl_->loop_param, &Impl::loop_callback,
        impl_.get(), &impl_->thread_ret);
    return impl_->thread_ctx != nullptr;
}

bool Client::wait_connected(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->cv.wait_for(lock, timeout,
                              [&] { return impl_->connected; });
}

bool Client::send(roqr::Frame /*frame*/, DeliveryMode /*mode*/) {
    return false;  // Task 5 implements the stream path, Task 6 datagrams.
}

void Client::bind_flow(uint64_t flow_id) {
    std::lock_guard lock(impl_->mutex);
    impl_->flows.activate(flow_id);  // full wiring in Task 7
}

void Client::retire_flow(uint64_t flow_id) {
    std::lock_guard lock(impl_->mutex);
    impl_->flows.retire(flow_id);
}

void Client::reset_flow_stream(uint64_t /*flow_id*/) {
    // Task 9.
}

void Client::close(roqr::ErrorCode code) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->close_requested = true;
        impl_->requested_close_code = static_cast<uint64_t>(code);
    }
    impl_->wake();
}

bool Client::wait_closed(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->cv.wait_for(lock, timeout, [&] { return impl_->closed; });
}

}  // namespace roqr::quic
