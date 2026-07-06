#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace roqr::gateway {

struct EgressOptions {
    uint16_t rtmp_port = 1936;
    std::string roqr_host = "127.0.0.1";
    uint16_t roqr_port = 4443;
    std::string stream_name = "cam";
    bool insecure_skip_verify = true;
};

// Accepts one RTMP player (ffplay) on rtmp_port, connects to the RoQR
// server, plays stream_name, and serves received media to the player.
// Applies draft s8 gap recovery: after a suspected datagram gap, drops
// non-keyframe video until the next keyframe/sequence header.
class EgressGateway {
public:
    EgressGateway();
    ~EgressGateway();
    bool start(const EgressOptions& options);
    void stop();
    bool wait_playing(std::chrono::milliseconds timeout);

    // Number of media messages dropped under player backpressure (draft s11).
    uint64_t frames_dropped() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace roqr::gateway
