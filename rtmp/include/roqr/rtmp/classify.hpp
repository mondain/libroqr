#pragma once

#include <cstdint>
#include <span>

namespace roqr::rtmp {

enum class MediaClass {
    SequenceHeader,  // decoder config: must travel reliably
    Metadata,        // onMetaData / VideoPacketType.Metadata
    Keyframe,        // random access point coded frame
    Coded,           // other coded media
    Control,         // protocol control / sequence end / info frames
    Unknown,
};

struct MediaInfo {
    MediaClass cls = MediaClass::Unknown;
    bool enhanced = false;      // E-RTMP ex-header present
    bool multitrack = false;    // E-RTMP multitrack message
    bool force_stream = false;  // conservative: carry reliably (draft s10)
    uint32_t fourcc = 0;        // E-RTMP FourCC as big-endian u32, else 0
    uint8_t codec = 0;          // legacy video codec id / audio sound format
};

MediaInfo classify_video(std::span<const uint8_t> payload);
MediaInfo classify_audio(std::span<const uint8_t> payload);
// Dispatch on RTMP message type: 8 audio, 9 video, 15/18 data->Metadata,
// 1-6 -> Control, 22 aggregate -> Coded; anything else Unknown+force_stream.
MediaInfo classify(uint8_t message_type, std::span<const uint8_t> payload);

}  // namespace roqr::rtmp
