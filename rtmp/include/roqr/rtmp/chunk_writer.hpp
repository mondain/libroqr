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
    explicit ChunkWriter(uint32_t chunk_size = kDefaultChunkSize)
        : chunk_size_(chunk_size) {}

    bool write(const RtmpMessage& msg, std::vector<uint8_t>& out);
    void set_chunk_size(uint32_t size, std::vector<uint8_t>& out);
    uint32_t chunk_size() const { return chunk_size_; }

private:
    uint32_t chunk_size_;
};

}  // namespace roqr::rtmp
