#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "roqr/varint.hpp"

namespace roqr {

// One RoQR frame: RTMP message metadata plus exactly one complete RTMP
// message payload (draft s7.2).
struct Frame {
    uint64_t flow_id = 0;
    uint64_t timestamp = 0;
    uint8_t message_type = 0;
    uint64_t message_stream_id = 0;
    uint64_t chunk_stream_id = 0;
    std::vector<uint8_t> payload;

    bool operator==(const Frame&) const = default;
};

// Appends the encoded frame to out. Returns false and leaves out untouched
// if any varint field exceeds kVarintMax or the payload is empty (draft
// s7.2: Payload Length MUST be greater than zero).
bool frame_encode(const Frame& frame, std::vector<uint8_t>& out);

enum class DecodeStatus { Ok, NeedMoreData, Malformed };

// Decodes a DATAGRAM-carried frame: data MUST contain exactly one complete
// frame and no trailing bytes (draft s7.5). Incomplete or trailing input is
// Malformed, never NeedMoreData.
DecodeStatus datagram_decode(std::span<const uint8_t> data, Frame& out);

// Incremental decoder for stream-carried frames (draft s7.4). Feed stream
// bytes as they arrive; complete frames become available via next().
class FrameDecoder {
public:
    static constexpr uint64_t kDefaultMaxPayload = 16ull * 1024 * 1024;

    // max_payload bounds the accepted Payload Length; larger values mark
    // the decoder malformed (resource guard, draft s14).
    explicit FrameDecoder(uint64_t max_payload = kDefaultMaxPayload);

    void feed(std::span<const uint8_t> data);
    std::optional<Frame> next();
    bool malformed() const { return malformed_; }

private:
    void parse();

    std::vector<uint8_t> buffer_;
    std::deque<Frame> ready_;
    uint64_t max_payload_;
    bool malformed_ = false;
};

}  // namespace roqr
