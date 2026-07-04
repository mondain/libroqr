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

struct Header {
    uint64_t flow_id = 0;
    uint64_t timestamp = 0;
    uint64_t message_stream_id = 0;
    uint64_t chunk_stream_id = 0;
    uint64_t payload_length = 0;
    uint8_t message_type = 0;
    size_t consumed = 0;
};

// Parses the fixed frame header. Returns NeedMoreData when data is too
// short; a header by itself is never Malformed (any byte sequence is a
// valid varint prefix).
DecodeStatus parse_header(std::span<const uint8_t> data, Header& h) {
    size_t off = 0;
    auto take = [&](uint64_t& out) {
        auto r = varint_decode(data.subspan(off));
        if (!r) return false;
        out = r->value;
        off += r->consumed;
        return true;
    };

    if (!take(h.flow_id) || !take(h.timestamp)) return DecodeStatus::NeedMoreData;
    if (off >= data.size()) return DecodeStatus::NeedMoreData;
    h.message_type = data[off++];
    if (!take(h.message_stream_id) || !take(h.chunk_stream_id) ||
        !take(h.payload_length)) {
        return DecodeStatus::NeedMoreData;
    }
    h.consumed = off;
    return DecodeStatus::Ok;
}

void header_to_frame(const Header& h, Frame& f) {
    f.flow_id = h.flow_id;
    f.timestamp = h.timestamp;
    f.message_type = h.message_type;
    f.message_stream_id = h.message_stream_id;
    f.chunk_stream_id = h.chunk_stream_id;
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
    Header h;
    if (parse_header(data, h) != DecodeStatus::Ok) return DecodeStatus::Malformed;
    if (h.payload_length == 0) return DecodeStatus::Malformed;
    if (data.size() - h.consumed != h.payload_length) return DecodeStatus::Malformed;

    header_to_frame(h, out);
    out.payload.assign(data.begin() + h.consumed, data.end());
    return DecodeStatus::Ok;
}

FrameDecoder::FrameDecoder(uint64_t max_payload) : max_payload_(max_payload) {}

void FrameDecoder::feed(std::span<const uint8_t> data) {
    if (malformed_) return;
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    parse();
}

std::optional<Frame> FrameDecoder::next() {
    if (ready_.empty()) return std::nullopt;
    Frame f = std::move(ready_.front());
    ready_.pop_front();
    return f;
}

void FrameDecoder::parse() {
    for (;;) {
        Header h;
        if (parse_header(buffer_, h) != DecodeStatus::Ok) return;
        if (h.payload_length == 0 || h.payload_length > max_payload_) {
            malformed_ = true;
            buffer_.clear();
            return;
        }
        if (buffer_.size() - h.consumed < h.payload_length) return;

        Frame f;
        header_to_frame(h, f);
        const auto payload_begin =
            buffer_.begin() + static_cast<ptrdiff_t>(h.consumed);
        const auto payload_end =
            payload_begin + static_cast<ptrdiff_t>(h.payload_length);
        f.payload.assign(payload_begin, payload_end);
        ready_.push_back(std::move(f));
        buffer_.erase(buffer_.begin(), payload_end);
    }
}

}  // namespace roqr
