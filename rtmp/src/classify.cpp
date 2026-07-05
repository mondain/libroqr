#include "roqr/rtmp/classify.hpp"

#include <optional>

#include "roqr/rtmp/message.hpp"

namespace roqr::rtmp {

namespace {

MediaInfo unknown_reliable() {
    MediaInfo info;
    info.cls = MediaClass::Unknown;
    info.force_stream = true;
    return info;
}

MediaInfo unknown_reliable_enhanced() {
    MediaInfo info = unknown_reliable();
    info.enhanced = true;
    return info;
}

// Skips ModEx blocks (packetType 7). On success updates pos and returns
// the effective packet type; returns nullopt on truncation.
std::optional<uint8_t> skip_modex(std::span<const uint8_t> p, size_t& pos,
                                   uint8_t packet_type) {
    while (packet_type == 7) {
        if (pos >= p.size()) return std::nullopt;
        size_t size = static_cast<size_t>(p[pos++]) + 1;
        if (size == 256) {
            if (pos + 2 > p.size()) return std::nullopt;
            size = (static_cast<size_t>(p[pos]) << 8 | p[pos + 1]) + 1;
            pos += 2;
        }
        if (pos + size + 1 > p.size()) return std::nullopt;
        pos += size;
        // High nibble: ModEx type (ignored). Low nibble: effective packetType.
        packet_type = p[pos++] & 0x0F;
    }
    return packet_type;
}

std::optional<uint32_t> read_fourcc(std::span<const uint8_t> p, size_t& pos) {
    if (pos + 4 > p.size()) return std::nullopt;
    const uint32_t f = static_cast<uint32_t>(p[pos]) << 24 |
                        static_cast<uint32_t>(p[pos + 1]) << 16 |
                        static_cast<uint32_t>(p[pos + 2]) << 8 | p[pos + 3];
    pos += 4;
    return f;
}

MediaInfo classify_video_enhanced(std::span<const uint8_t> p) {
    MediaInfo info;
    info.enhanced = true;
    const uint8_t frame_type = (p[0] >> 4) & 0x07;
    size_t pos = 1;

    auto packet_type = skip_modex(p, pos, p[0] & 0x0F);
    if (!packet_type) return unknown_reliable_enhanced();

    if (*packet_type == 6) {  // Multitrack: conservative
        info.multitrack = true;
        info.force_stream = true;
        info.cls = MediaClass::Coded;
        if (pos < p.size()) {
            const uint8_t av_multitrack_type = p[pos] >> 4;
            ++pos;
            if (av_multitrack_type != 2) {  // not ManyTracksManyCodecs
                if (auto f = read_fourcc(p, pos)) info.fourcc = *f;
            }
        }
        return info;
    }

    auto f = read_fourcc(p, pos);
    if (!f) return unknown_reliable_enhanced();
    info.fourcc = *f;

    switch (*packet_type) {
        case 0:  // SequenceStart
        case 5:  // MPEG2TSSequenceStart
            info.cls = MediaClass::SequenceHeader;
            break;
        case 1:  // CodedFrames
        case 3:  // CodedFramesX
            info.cls = frame_type == 1 ? MediaClass::Keyframe
                                       : MediaClass::Coded;
            break;
        case 2:  // SequenceEnd
            info.cls = MediaClass::Control;
            break;
        case 4:  // Metadata
            info.cls = MediaClass::Metadata;
            break;
        default:
            return unknown_reliable_enhanced();
    }
    return info;
}

MediaInfo classify_audio_enhanced(std::span<const uint8_t> p) {
    MediaInfo info;
    info.enhanced = true;
    size_t pos = 1;

    auto packet_type = skip_modex(p, pos, p[0] & 0x0F);
    if (!packet_type) return unknown_reliable_enhanced();

    if (*packet_type == 5) {  // Multitrack: conservative
        info.multitrack = true;
        info.force_stream = true;
        info.cls = MediaClass::Coded;
        if (pos < p.size()) {
            const uint8_t av_multitrack_type = p[pos] >> 4;
            ++pos;
            if (av_multitrack_type != 2) {
                if (auto f = read_fourcc(p, pos)) info.fourcc = *f;
            }
        }
        return info;
    }

    auto f = read_fourcc(p, pos);
    if (!f) return unknown_reliable_enhanced();
    info.fourcc = *f;

    switch (*packet_type) {
        case 0:  // SequenceStart
        case 4:  // MultichannelConfig
            info.cls = MediaClass::SequenceHeader;
            break;
        case 1:  // CodedFrames
            info.cls = MediaClass::Coded;
            break;
        case 2:  // SequenceEnd
            info.cls = MediaClass::Control;
            break;
        default:
            return unknown_reliable_enhanced();
    }
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
