#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace roqr::gateway {

struct IngestOptions {
    uint16_t rtmp_port = 1935;
    std::string roqr_host = "127.0.0.1";
    uint16_t roqr_port = 4443;
    bool insecure_skip_verify = true;
};

// Accepts one RTMP publisher on rtmp_port, re-originates its session over a
// RoQR connection to the server, and forwards media. One publisher at a
// time (gateway-grade). start() returns after the RTMP listener is up.
class IngestGateway {
public:
    IngestGateway();
    ~IngestGateway();  // stop()
    bool start(const IngestOptions& options);
    void stop();
    // True once an RTMP publisher connected AND the RoQR publish handshake
    // completed. For tests.
    bool wait_publishing(std::chrono::milliseconds timeout);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace roqr::gateway
