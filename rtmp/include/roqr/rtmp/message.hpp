#pragma once

#include <cstdint>
#include <vector>

namespace roqr::rtmp {

// One de-chunked RTMP message (draft s7.3: extended timestamps already
// resolved into the timestamp field).
struct RtmpMessage {
    uint32_t timestamp = 0;
    uint8_t type = 0;
    uint32_t message_stream_id = 0;
    uint32_t chunk_stream_id = 0;
    std::vector<uint8_t> payload;

    bool operator==(const RtmpMessage&) const = default;
};

inline constexpr uint32_t kDefaultChunkSize = 128;

// RTMP message type ids used by this module.
inline constexpr uint8_t kTypeSetChunkSize = 1;
inline constexpr uint8_t kTypeAbort = 2;
inline constexpr uint8_t kTypeAcknowledgement = 3;
inline constexpr uint8_t kTypeUserControl = 4;
inline constexpr uint8_t kTypeWindowAckSize = 5;
inline constexpr uint8_t kTypeSetPeerBandwidth = 6;
inline constexpr uint8_t kTypeAudio = 8;
inline constexpr uint8_t kTypeVideo = 9;
inline constexpr uint8_t kTypeDataAmf0 = 18;
inline constexpr uint8_t kTypeCommandAmf0 = 20;

}  // namespace roqr::rtmp
