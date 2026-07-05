#include <catch2/catch_test_macros.hpp>

#include "roqr/rtmp/classify.hpp"
#include "roqr/rtmp/message.hpp"

using namespace roqr::rtmp;

TEST_CASE("legacy AVC video classifies by frame and packet type") {
    const uint8_t seq[] = {0x17, 0x00, 0x00, 0x00, 0x00};
    auto info = classify_video(seq);
    CHECK(info.cls == MediaClass::SequenceHeader);
    CHECK(info.codec == 7);
    CHECK_FALSE(info.enhanced);

    const uint8_t key[] = {0x17, 0x01, 0x00, 0x00, 0x00};
    CHECK(classify_video(key).cls == MediaClass::Keyframe);

    const uint8_t inter[] = {0x27, 0x01, 0x00, 0x00, 0x00};
    CHECK(classify_video(inter).cls == MediaClass::Coded);

    const uint8_t eos[] = {0x17, 0x02};
    CHECK(classify_video(eos).cls == MediaClass::Control);

    const uint8_t info_frame[] = {0x57, 0x00};  // frame type 5
    CHECK(classify_video(info_frame).cls == MediaClass::Control);
}

TEST_CASE("legacy AAC audio classifies sequence header vs raw") {
    const uint8_t seq[] = {0xAF, 0x00, 0x12, 0x10};
    auto info = classify_audio(seq);
    CHECK(info.cls == MediaClass::SequenceHeader);
    CHECK(info.codec == 10);

    const uint8_t raw[] = {0xAF, 0x01, 0x21};
    CHECK(classify_audio(raw).cls == MediaClass::Coded);

    const uint8_t mp3[] = {0x2F, 0x11};  // sound format 2
    CHECK(classify_audio(mp3).cls == MediaClass::Coded);
}

TEST_CASE("classify dispatches by message type") {
    const uint8_t meta[] = {0x02};
    CHECK(classify(kTypeDataAmf0, meta).cls == MediaClass::Metadata);
    CHECK(classify(15, meta).cls == MediaClass::Metadata);
    CHECK(classify(kTypeSetChunkSize, meta).cls == MediaClass::Control);
    CHECK(classify(kTypeUserControl, meta).cls == MediaClass::Control);
    CHECK(classify(22, meta).cls == MediaClass::Coded);
    auto unknown = classify(200, meta);
    CHECK(unknown.cls == MediaClass::Unknown);
    CHECK(unknown.force_stream);
}

TEST_CASE("ex-header and empty payloads are conservative until Task 7") {
    const uint8_t exv[] = {0x90, 'h', 'v', 'c', '1'};
    auto v = classify_video(exv);
    CHECK(v.enhanced);
    CHECK(v.force_stream);

    const uint8_t exa[] = {0x90, 'a', 'c', '-', '3'};
    auto a = classify_audio(exa);
    CHECK(a.enhanced);
    CHECK(a.force_stream);

    CHECK(classify_video({}).force_stream);
    CHECK(classify_audio({}).force_stream);
}
