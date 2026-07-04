#include "roqr/rtmp/handshake.hpp"

#include <algorithm>
#include <random>

namespace roqr::rtmp {

namespace {

std::vector<uint8_t> make_packet() {
    std::vector<uint8_t> pkt(kHandshakePacketSize, 0);
    // time(4) and zero(4) stay 0; fill the 1528 random bytes.
    std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 8; i < pkt.size(); ++i) {
        pkt[i] = static_cast<uint8_t>(dist(rng));
    }
    return pkt;
}

// C2/S2: peer time(4) + local time(4, we send 0) + peer random echo.
std::vector<uint8_t> make_echo(std::span<const uint8_t> peer_packet) {
    std::vector<uint8_t> echo(peer_packet.begin(), peer_packet.end());
    std::fill(echo.begin() + 4, echo.begin() + 8, 0);
    return echo;
}

bool random_matches(std::span<const uint8_t> echo,
                    std::span<const uint8_t> original) {
    return std::equal(echo.begin() + 8, echo.end(), original.begin() + 8);
}

}  // namespace

bool HandshakeResponder::feed(std::span<const uint8_t> in,
                              std::vector<uint8_t>& out) {
    if (state_ == State::Failed) return false;
    buffer_.insert(buffer_.end(), in.begin(), in.end());

    if (state_ == State::WaitC0C1) {
        if (buffer_.empty()) return true;
        if (buffer_[0] != kRtmpVersion) {
            state_ = State::Failed;
            return false;
        }
        if (buffer_.size() < 1 + kHandshakePacketSize) return true;
        const std::span<const uint8_t> c1(buffer_.data() + 1,
                                          kHandshakePacketSize);
        s1_ = make_packet();
        out.push_back(kRtmpVersion);                    // S0
        out.insert(out.end(), s1_.begin(), s1_.end());  // S1
        const auto s2 = make_echo(c1);                  // S2
        out.insert(out.end(), s2.begin(), s2.end());
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + 1 + kHandshakePacketSize);
        state_ = State::WaitC2;
    }
    if (state_ == State::WaitC2) {
        if (buffer_.size() < kHandshakePacketSize) return true;
        const std::span<const uint8_t> c2(buffer_.data(),
                                          kHandshakePacketSize);
        if (!random_matches(c2, s1_)) {
            state_ = State::Failed;
            return false;
        }
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + kHandshakePacketSize);
        state_ = State::Done;
    }
    return true;
}

std::vector<uint8_t> HandshakeInitiator::start() {
    c1_ = make_packet();
    std::vector<uint8_t> out;
    out.push_back(kRtmpVersion);
    out.insert(out.end(), c1_.begin(), c1_.end());
    state_ = State::WaitS0S1S2;
    return out;
}

bool HandshakeInitiator::feed(std::span<const uint8_t> in,
                              std::vector<uint8_t>& out) {
    if (state_ == State::Failed) return false;
    if (state_ != State::WaitS0S1S2) return state_ == State::Done;
    buffer_.insert(buffer_.end(), in.begin(), in.end());

    if (!buffer_.empty() && buffer_[0] != kRtmpVersion) {
        state_ = State::Failed;
        return false;
    }
    if (buffer_.size() < 1 + 2 * kHandshakePacketSize) return true;

    const std::span<const uint8_t> s1(buffer_.data() + 1,
                                      kHandshakePacketSize);
    const std::span<const uint8_t> s2(
        buffer_.data() + 1 + kHandshakePacketSize, kHandshakePacketSize);
    if (!random_matches(s2, c1_)) {
        state_ = State::Failed;
        return false;
    }
    const auto c2 = make_echo(s1);
    out.insert(out.end(), c2.begin(), c2.end());
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + 1 + 2 * kHandshakePacketSize);
    state_ = State::Done;
    return true;
}

}  // namespace roqr::rtmp
