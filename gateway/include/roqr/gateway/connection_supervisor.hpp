#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "roqr/quic/client.hpp"

namespace roqr::gateway {

struct ReconnectPolicy {
    std::chrono::milliseconds initial_backoff{250};
    std::chrono::milliseconds max_backoff{5000};
    // Consecutive FAILED connect attempts before giving up (failed() = true).
    // 0 = never give up. A successful connection resets the counter.
    unsigned max_attempts = 10;
    // Per-attempt wait for the connection to come up before counting a failure.
    std::chrono::milliseconds connect_timeout{5000};
};

// Keeps a roqr::quic::Client connected to one server, rebuilding it after a
// drop. One background supervisor thread runs the connect -> serve -> backoff
// loop; the QUIC Client is one-shot so each (re)connection is a fresh Client.
class ConnectionSupervisor {
public:
    // on_ready(Client&): invoked on the supervisor thread each time a fresh
    //   connection is confirmed up — send the session handshake here
    //   (connect/createStream/publish|play). Must not block (same rule as any
    //   Client handler).
    // on_message: wired as each Client's message handler.
    using ReadyHandler = std::function<void(roqr::quic::Client&)>;
    using MessageHandler = roqr::quic::Client::MessageHandler;

    ConnectionSupervisor(std::string host, uint16_t port,
                         roqr::quic::ClientOptions opts,
                         ReconnectPolicy policy,
                         ReadyHandler on_ready,
                         MessageHandler on_message);
    ~ConnectionSupervisor();  // calls stop()
    ConnectionSupervisor(const ConnectionSupervisor&) = delete;
    ConnectionSupervisor& operator=(const ConnectionSupervisor&) = delete;

    void start();  // spawn the supervisor thread; idempotent
    void stop();   // stop the loop, tear down the current Client, join; idempotent

    // Send on the current connection. Returns false if not currently connected
    // (the frame is dropped — live media, no buffering).
    bool send(roqr::Frame frame, roqr::quic::DeliveryMode mode);

    bool connected() const;         // a connection is currently up
    bool failed() const;            // gave up after max_attempts consecutive failures
    uint64_t reconnect_count() const;  // successful reconnects AFTER the first connect

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace roqr::gateway
