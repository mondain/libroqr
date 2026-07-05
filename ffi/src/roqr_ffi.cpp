#include "roqr/roqr.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <new>

#include "roqr/error.hpp"
#include "roqr/quic/client.hpp"

// The opaque roqr_client owns a roqr::quic::Client plus the C callback
// pointers registered against it (added in Task 2).
struct roqr_client {
    roqr::quic::Client client;
    roqr_message_cb on_message = nullptr;
    void* on_message_user = nullptr;
    roqr_closed_cb on_closed = nullptr;
    void* on_closed_user = nullptr;
};

namespace {

roqr::quic::DeliveryMode to_cpp_mode(roqr_delivery_mode m) {
    switch (m) {
        case ROQR_DELIVERY_DATAGRAM:
            return roqr::quic::DeliveryMode::Datagram;
        case ROQR_DELIVERY_AUTO:
            return roqr::quic::DeliveryMode::Auto;
        case ROQR_DELIVERY_STREAM:
        default:
            return roqr::quic::DeliveryMode::Stream;
    }
}

}  // namespace

extern "C" {

const char* roqr_version(void) { return "0.1.0"; }

roqr_client* roqr_client_create(void) {
    return new (std::nothrow) roqr_client();
}

void roqr_client_destroy(roqr_client* client) { delete client; }

void roqr_client_set_on_message(roqr_client* c, roqr_message_cb cb,
                                void* user) {
    if (c == nullptr) return;
    c->on_message = cb;
    c->on_message_user = user;
}

void roqr_client_set_on_closed(roqr_client* c, roqr_closed_cb cb,
                               void* user) {
    if (c == nullptr) return;
    c->on_closed = cb;
    c->on_closed_user = user;
}

roqr_error roqr_client_connect(roqr_client* c, const char* host,
                               uint16_t port, int insecure_skip_verify) {
    if (c == nullptr || host == nullptr) return ROQR_ERR_INVALID_ARG;

    c->client.on_message([c](const roqr::Frame& f) {
        if (c->on_message == nullptr) return;
        roqr_frame cf{};
        cf.flow_id = f.flow_id;
        cf.timestamp = f.timestamp;
        cf.message_type = f.message_type;
        cf.message_stream_id = f.message_stream_id;
        cf.chunk_stream_id = f.chunk_stream_id;
        cf.payload = f.payload.data();
        cf.payload_len = f.payload.size();
        c->on_message(&cf, c->on_message_user);
    });
    c->client.on_closed([c](uint64_t code) {
        if (c->on_closed != nullptr) c->on_closed(code, c->on_closed_user);
    });

    roqr::quic::ClientOptions opts;
    opts.insecure_skip_verify = insecure_skip_verify != 0;
    if (!c->client.connect(host, port, opts)) return ROQR_ERR_CONNECT_FAILED;
    return ROQR_OK;
}

int roqr_client_wait_connected(roqr_client* c, int timeout_ms) {
    if (c == nullptr) return 0;
    return c->client.wait_connected(std::chrono::milliseconds(timeout_ms))
               ? 1
               : 0;
}

int roqr_client_datagrams_negotiated(roqr_client* c) {
    return (c != nullptr && c->client.datagrams_negotiated()) ? 1 : 0;
}

roqr_error roqr_client_send(roqr_client* c, const roqr_frame* frame,
                            roqr_delivery_mode mode) {
    if (c == nullptr || frame == nullptr || frame->payload_len == 0) {
        return ROQR_ERR_INVALID_ARG;
    }
    roqr::Frame f;
    f.flow_id = frame->flow_id;
    f.timestamp = frame->timestamp;
    f.message_type = frame->message_type;
    f.message_stream_id = frame->message_stream_id;
    f.chunk_stream_id = frame->chunk_stream_id;
    f.payload.assign(frame->payload, frame->payload + frame->payload_len);
    return c->client.send(std::move(f), to_cpp_mode(mode)) ? ROQR_OK
                                                           : ROQR_ERR_GENERAL;
}

void roqr_client_bind_flow(roqr_client* c, uint64_t flow_id) {
    if (c != nullptr) c->client.bind_flow(flow_id);
}

void roqr_client_retire_flow(roqr_client* c, uint64_t flow_id) {
    if (c != nullptr) c->client.retire_flow(flow_id);
}

void roqr_client_close(roqr_client* c, uint64_t app_error_code) {
    if (c != nullptr) {
        c->client.close(static_cast<roqr::ErrorCode>(app_error_code));
    }
}

int roqr_client_wait_closed(roqr_client* c, int timeout_ms) {
    if (c == nullptr) return 0;
    return c->client.wait_closed(std::chrono::milliseconds(timeout_ms)) ? 1
                                                                        : 0;
}

}  // extern "C"
