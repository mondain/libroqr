#include "roqr/rtmp/chunk_writer.hpp"

#include <algorithm>

namespace roqr::rtmp {

namespace {

void put_u24(uint32_t v, std::vector<uint8_t>& out) {
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put_u32be(uint32_t v, std::vector<uint8_t>& out) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put_u32le(uint32_t v, std::vector<uint8_t>& out) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

void put_basic_header(uint8_t fmt, uint32_t csid, std::vector<uint8_t>& out) {
    if (csid < 64) {
        out.push_back(static_cast<uint8_t>(fmt << 6 | csid));
    } else if (csid < 320) {
        out.push_back(static_cast<uint8_t>(fmt << 6));
        out.push_back(static_cast<uint8_t>(csid - 64));
    } else {
        out.push_back(static_cast<uint8_t>(fmt << 6 | 1));
        out.push_back(static_cast<uint8_t>((csid - 64) & 0xFF));
        out.push_back(static_cast<uint8_t>((csid - 64) >> 8));
    }
}

}  // namespace

bool ChunkWriter::write(const RtmpMessage& msg, std::vector<uint8_t>& out) {
    if (msg.chunk_stream_id < 2 || msg.chunk_stream_id > 65599) return false;

    const bool extended = msg.timestamp >= 0xFFFFFF;

    put_basic_header(0, msg.chunk_stream_id, out);
    put_u24(extended ? 0xFFFFFF : msg.timestamp, out);
    put_u24(static_cast<uint32_t>(msg.payload.size()), out);
    out.push_back(msg.type);
    put_u32le(msg.message_stream_id, out);
    if (extended) put_u32be(msg.timestamp, out);

    size_t offset = 0;
    for (;;) {
        const size_t take =
            std::min<size_t>(chunk_size_, msg.payload.size() - offset);
        out.insert(out.end(), msg.payload.begin() + static_cast<ptrdiff_t>(offset),
                   msg.payload.begin() + static_cast<ptrdiff_t>(offset + take));
        offset += take;
        if (offset >= msg.payload.size()) break;
        put_basic_header(3, msg.chunk_stream_id, out);
        if (extended) put_u32be(msg.timestamp, out);
    }
    return true;
}

void ChunkWriter::set_chunk_size(uint32_t size, std::vector<uint8_t>& out) {
    RtmpMessage m;
    m.chunk_stream_id = 2;
    m.type = kTypeSetChunkSize;
    m.message_stream_id = 0;
    put_u32be(size, m.payload);
    write(m, out);
    chunk_size_ = size;
}

}  // namespace roqr::rtmp
