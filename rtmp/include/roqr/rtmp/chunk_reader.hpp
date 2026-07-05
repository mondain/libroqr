#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "roqr/rtmp/message.hpp"

namespace roqr::rtmp {

// Incremental RTMP dechunker. Feed raw TCP bytes; complete messages come
// out of next() in wire order. Set Chunk Size and Abort are applied
// internally and also surfaced as messages.
class ChunkReader {
public:
    static constexpr uint32_t kMaxMessageSize = 8 * 1024 * 1024;  // must be < 0xFFFFFF+1 so a 24-bit length can exceed it

    void feed(std::span<const uint8_t> data);
    std::optional<RtmpMessage> next();
    bool failed() const { return failed_; }
    uint32_t chunk_size() const { return chunk_size_; }

private:
    struct CsidState {
        uint32_t timestamp = 0;
        uint32_t delta = 0;
        uint32_t length = 0;
        uint32_t message_stream_id = 0;
        uint8_t type = 0;
        bool extended = false;
        bool have_header = false;
        std::vector<uint8_t> assembling;
        uint32_t remaining = 0;
    };

    void parse();
    void finalize(uint32_t csid, CsidState& st);

    std::vector<uint8_t> buffer_;
    std::deque<RtmpMessage> ready_;
    std::map<uint32_t, CsidState> streams_;
    uint32_t chunk_size_ = kDefaultChunkSize;
    bool failed_ = false;
};

}  // namespace roqr::rtmp
