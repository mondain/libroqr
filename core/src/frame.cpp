#include "roqr/frame.hpp"

namespace roqr {

namespace {

bool append_varint(uint64_t value, std::vector<uint8_t>& out) {
    uint8_t tmp[8];
    const size_t n = varint_encode(value, tmp);
    if (n == 0) return false;
    out.insert(out.end(), tmp, tmp + n);
    return true;
}

}  // namespace

bool frame_encode(const Frame& frame, std::vector<uint8_t>& out) {
    if (frame.payload.empty()) return false;

    const size_t start = out.size();
    const bool ok = append_varint(frame.flow_id, out) &&
                    append_varint(frame.timestamp, out) &&
                    (out.push_back(frame.message_type), true) &&
                    append_varint(frame.message_stream_id, out) &&
                    append_varint(frame.chunk_stream_id, out) &&
                    append_varint(frame.payload.size(), out);
    if (!ok) {
        out.resize(start);
        return false;
    }
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    return true;
}

DecodeStatus datagram_decode(std::span<const uint8_t> data, Frame& out) {
    // Implemented in the next task.
    (void)data;
    (void)out;
    return DecodeStatus::Malformed;
}

FrameDecoder::FrameDecoder(uint64_t max_payload) : max_payload_(max_payload) {}

void FrameDecoder::feed(std::span<const uint8_t> data) {
    // Implemented in Task 6.
    (void)data;
}

std::optional<Frame> FrameDecoder::next() {
    return std::nullopt;
}

void FrameDecoder::parse() {}

}  // namespace roqr
