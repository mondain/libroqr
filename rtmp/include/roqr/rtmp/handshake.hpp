#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace roqr::rtmp {

inline constexpr size_t kHandshakePacketSize = 1536;
inline constexpr uint8_t kRtmpVersion = 3;

// RTMP simple (unencrypted) handshake, sans-I/O. Both classes buffer
// fragmented input internally; feed() appends any bytes that must be sent
// to the peer and returns false on protocol failure (latched).

class HandshakeResponder {
public:
    bool feed(std::span<const uint8_t> in, std::vector<uint8_t>& out);
    bool done() const { return state_ == State::Done; }
    // Returns and clears any bytes received beyond the handshake (e.g.
    // pipelined RTMP chunk data). Call after done().
    std::vector<uint8_t> take_leftover();

private:
    enum class State { WaitC0C1, WaitC2, Done, Failed };
    State state_ = State::WaitC0C1;
    std::vector<uint8_t> buffer_;
    std::vector<uint8_t> s1_;
};

class HandshakeInitiator {
public:
    std::vector<uint8_t> start();
    bool feed(std::span<const uint8_t> in, std::vector<uint8_t>& out);
    bool done() const { return state_ == State::Done; }
    // Returns and clears any bytes received beyond the handshake (e.g.
    // pipelined RTMP chunk data). Call after done().
    std::vector<uint8_t> take_leftover();

private:
    enum class State { Idle, WaitS0S1S2, Done, Failed };
    State state_ = State::Idle;
    std::vector<uint8_t> buffer_;
    std::vector<uint8_t> c1_;
};

}  // namespace roqr::rtmp
