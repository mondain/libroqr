#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "roqr/error.hpp"
#include "roqr/flow_table.hpp"
#include "roqr/frame.hpp"
#include "roqr/quic/delivery.hpp"

namespace roqr::quic {

struct ClientOptions {
    std::string alpn = "roqr";
    bool insecure_skip_verify = true;
    DatagramFallback datagram_fallback = DatagramFallback::Stream;
    roqr::FlowTableLimits flow_limits{};

    // 0 keeps picoquic's default (30s). Bounds how long the client waits for
    // peer activity before declaring the connection dead — this is what
    // lets a supervisor notice a silently-dropped server (the relay sends
    // no CONNECTION_CLOSE on shutdown). Keepalive is enabled at
    // idle_timeout/2 so an idle-but-alive connection is not killed by a
    // short timeout.
    std::chrono::milliseconds idle_timeout{15000};
    // 0 keeps picoquic's default. Bounds how long connect()/wait_connected
    // wait for the handshake before a failed attempt; keep it well under
    // idle_timeout.
    std::chrono::milliseconds handshake_timeout{10000};
};

// RoQR client over picoquic. One background network thread owns all
// picoquic calls; handlers fire on that thread and must not block.
// send()/close()/bind_flow()/retire_flow() are safe from any thread.
class Client {
public:
    using MessageHandler = std::function<void(const roqr::Frame&)>;
    using ClosedHandler = std::function<void(uint64_t app_error_code)>;

    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Set handlers before connect(); they run on the network thread.
    void on_message(MessageHandler h);
    void on_closed(ClosedHandler h);

    bool connect(const std::string& host, uint16_t port,
                 ClientOptions options = {});
    bool wait_connected(std::chrono::milliseconds timeout);

    // True once the connection is ready and the peer accepted the QUIC
    // DATAGRAM extension (draft s4).
    bool datagrams_negotiated() const;

    // Number of RoQR frames sent in QUIC DATAGRAM frames so far.
    uint64_t datagrams_sent() const;
    // Frames dropped by Datagram-mode policy or queue failure.
    uint64_t datagrams_dropped() const;

    bool send(roqr::Frame frame, DeliveryMode mode);
    void bind_flow(uint64_t flow_id);
    void retire_flow(uint64_t flow_id);
    void reset_flow_stream(uint64_t flow_id);

    void close(roqr::ErrorCode code = roqr::ErrorCode::NoError);
    bool wait_closed(std::chrono::milliseconds timeout);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace roqr::quic
