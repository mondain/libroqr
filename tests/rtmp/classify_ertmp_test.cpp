#include <catch2/catch_test_macros.hpp>

#include "roqr/rtmp/classify.hpp"

using namespace roqr::rtmp;

namespace {
constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24 |
           static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16 |
           static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8 |
           static_cast<uint8_t>(d);
}
}  // namespace

TEST_CASE("enhanced video sequence start extracts fourcc") {
    // IsExHeader | frameType 1 | packetType 0 (SequenceStart) + 'hvc1'
    const uint8_t p[] = {0x90, 'h', 'v', 'c', '1', 0x01, 0x02};
    auto info = classify_video(p);
    CHECK(info.cls == MediaClass::SequenceHeader);
    CHECK(info.enhanced);
    CHECK(info.fourcc == fourcc('h', 'v', 'c', '1'));
    CHECK_FALSE(info.force_stream);
}

TEST_CASE("enhanced video coded frames keyframe vs inter") {
    // frameType 1 keyframe, packetType 1 CodedFrames, av01
    const uint8_t key[] = {0x91, 'a', 'v', '0', '1', 0xDE};
    auto k = classify_video(key);
    CHECK(k.cls == MediaClass::Keyframe);
    CHECK(k.fourcc == fourcc('a', 'v', '0', '1'));

    // frameType 2 inter, packetType 3 CodedFramesX, vp09
    const uint8_t inter[] = {0xA3, 'v', 'p', '0', '9', 0xDE};
    CHECK(classify_video(inter).cls == MediaClass::Coded);
}

TEST_CASE("enhanced video metadata and sequence end") {
    const uint8_t meta[] = {0x94, 'h', 'v', 'c', '1', 0x02};
    CHECK(classify_video(meta).cls == MediaClass::Metadata);

    const uint8_t end[] = {0x92, 'h', 'v', 'c', '1'};
    CHECK(classify_video(end).cls == MediaClass::Control);
}

TEST_CASE("modex blocks are skipped to reach the effective packet type") {
    // packetType 7 (ModEx): size byte 0x02 -> 3 bytes of ModEx data,
    // then high nibble = ModEx type, low nibble = packetType 1
    // (CodedFrames), then fourcc.
    const uint8_t p[] = {0x97, 0x02, 0x00, 0x00, 0x00,
                         0x01, 'h',  'v',  'c',  '1', 0xAB};
    auto info = classify_video(p);
    CHECK(info.cls == MediaClass::Keyframe);  // frameType 1
    CHECK(info.enhanced);
    CHECK(info.fourcc == fourcc('h', 'v', 'c', '1'));
}

TEST_CASE("multitrack is conservative and forces stream carriage") {
    // packetType 6 (Multitrack); next byte: AvMultitrackType 0 (OneTrack)
    // << 4 | inner packetType 1; then shared fourcc.
    const uint8_t p[] = {0x96, 0x01, 'h', 'v', 'c', '1', 0x00};
    auto info = classify_video(p);
    CHECK(info.multitrack);
    CHECK(info.force_stream);
    CHECK(info.enhanced);
    CHECK(info.fourcc == fourcc('h', 'v', 'c', '1'));
}

TEST_CASE("enhanced audio packet types") {
    // soundFormat 9 (ExHeader) << 4 | AudioPacketType 0 SequenceStart
    const uint8_t seq[] = {0x90, 'a', 'c', '-', '3', 0x01};
    auto s = classify_audio(seq);
    CHECK(s.cls == MediaClass::SequenceHeader);
    CHECK(s.enhanced);
    CHECK(s.fourcc == fourcc('a', 'c', '-', '3'));

    const uint8_t coded[] = {0x91, 'O', 'p', 'u', 's', 0x00};
    CHECK(classify_audio(coded).cls == MediaClass::Coded);

    const uint8_t multichannel[] = {0x94, 'f', 'l', 'a', 'c', 0x02};
    CHECK(classify_audio(multichannel).cls == MediaClass::SequenceHeader);

    // AudioPacketType 5 Multitrack.
    const uint8_t multitrack[] = {0x95, 0x01, 'a', 'c', '-', '3'};
    auto m = classify_audio(multitrack);
    CHECK(m.multitrack);
    CHECK(m.force_stream);
}

TEST_CASE("truncated ex-headers degrade to Unknown reliable") {
    const uint8_t only_header[] = {0x90};  // fourcc missing
    auto v = classify_video(only_header);
    CHECK(v.cls == MediaClass::Unknown);
    CHECK(v.force_stream);

    const uint8_t modex_trunc[] = {0x97, 0x10, 0x00};  // ModEx data cut off
    CHECK(classify_video(modex_trunc).cls == MediaClass::Unknown);
}
