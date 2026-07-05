#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "roqr/rtmp/message.hpp"

namespace roqr::rtmp {

// Chunks RtmpMessages for the wire: fmt0 header on the first chunk, fmt3
// continuations after, extended timestamps repeated on continuations.
class ChunkWriter {
public:
    // Constructor. If chunk_size is 0, it defaults to kDefaultChunkSize.
    explicit ChunkWriter(uint32_t chunk_size = kDefaultChunkSize)
        : chunk_size_(chunk_size == 0 ? kDefaultChunkSize : chunk_size) {}

    // Writes an RtmpMessage to the output buffer. Returns false if the message
    // is invalid (chunk_stream_id out of range, or payload > 16 MiB).
    bool write(const RtmpMessage& msg, std::vector<uint8_t>& out);

    // Sets a new chunk size for chunking outgoing messages. Ignores invalid
    // sizes (0 or with high bit set). Emits a control message on the wire.
    void set_chunk_size(uint32_t size, std::vector<uint8_t>& out);

    uint32_t chunk_size() const { return chunk_size_; }

private:
    uint32_t chunk_size_;
};

}  // namespace roqr::rtmp
