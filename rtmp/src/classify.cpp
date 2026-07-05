#include "roqr/rtmp/classify.hpp"

#include "roqr/rtmp/message.hpp"

namespace roqr::rtmp {

namespace {

MediaInfo unknown_reliable() {
    MediaInfo info;
    info.cls = MediaClass::Unknown;
    info.force_stream = true;
    return info;
}

// Task 7 replaces these with real E-RTMP v1/v2 parsing.
MediaInfo classify_video_enhanced(std::span<const uint8_t> payload) {
    (void)payload;
    MediaInfo info = unknown_reliable();
    info.enhanced = true;
    return info;
}

MediaInfo classify_audio_enhanced(std::span<const uint8_t> payload) {
    (void)payload;
    MediaInfo info = unknown_reliable();
    info.enhanced = true;
    return info;
}

}  // namespace

MediaInfo classify_video(std::span<const uint8_t> payload) {
    if (payload.empty()) return unknown_reliable();
    const uint8_t b0 = payload[0];
    if ((b0 & 0x80) != 0) return classify_video_enhanced(payload);

    MediaInfo info;
    const uint8_t frame_type = (b0 >> 4) & 0x07;
    info.codec = b0 & 0x0F;

    if (frame_type == 5) {  // video info / command frame
        info.cls = MediaClass::Control;
        return info;
    }
    if (info.codec == 7) {  // AVC: b1 is AVCPacketType
        if (payload.size() < 2) return unknown_reliable();
        if (payload[1] == 0) {
            info.cls = MediaClass::SequenceHeader;
            return info;
        }
        if (payload[1] == 2) {
            info.cls = MediaClass::Control;  // end of sequence
            return info;
        }
    }
    info.cls = frame_type == 1 ? MediaClass::Keyframe : MediaClass::Coded;
    return info;
}

MediaInfo classify_audio(std::span<const uint8_t> payload) {
    if (payload.empty()) return unknown_reliable();
    const uint8_t sound_format = payload[0] >> 4;
    if (sound_format == 9) return classify_audio_enhanced(payload);

    MediaInfo info;
    info.codec = sound_format;
    if (sound_format == 10) {  // AAC: b1 is AACPacketType
        if (payload.size() < 2) return unknown_reliable();
        info.cls = payload[1] == 0 ? MediaClass::SequenceHeader
                                   : MediaClass::Coded;
        return info;
    }
    info.cls = MediaClass::Coded;
    return info;
}

MediaInfo classify(uint8_t message_type, std::span<const uint8_t> payload) {
    switch (message_type) {
        case kTypeAudio:
            return classify_audio(payload);
        case kTypeVideo:
            return classify_video(payload);
        case 15:  // AMF3 data
        case kTypeDataAmf0: {
            MediaInfo info;
            info.cls = MediaClass::Metadata;
            return info;
        }
        case kTypeSetChunkSize:
        case kTypeAbort:
        case kTypeAcknowledgement:
        case kTypeUserControl:
        case kTypeWindowAckSize:
        case kTypeSetPeerBandwidth: {
            MediaInfo info;
            info.cls = MediaClass::Control;
            return info;
        }
        case 22: {  // aggregate: opaque coded media (spec: unpacking out of scope)
            MediaInfo info;
            info.cls = MediaClass::Coded;
            return info;
        }
        default:
            return unknown_reliable();
    }
}

}  // namespace roqr::rtmp
