#include <catch2/catch_test_macros.hpp>

#include "roqr/frame.hpp"

using namespace roqr;

namespace {

Frame sample_video_frame() {
    Frame f;
    f.flow_id = 0;
    f.timestamp = 1000;
    f.message_type = 9;  // RTMP Video
    f.message_stream_id = 1;
    f.chunk_stream_id = 4;
    f.payload = {0xDE, 0xAD};
    return f;
}

}  // namespace

TEST_CASE("frame_encode produces the draft s7.2 layout") {
    std::vector<uint8_t> out;
    REQUIRE(frame_encode(sample_video_frame(), out));

    // flow_id 0 -> 00; timestamp 1000 -> 2-byte varint 43 e8; type 09;
    // msg stream id 1 -> 01; chunk stream id 4 -> 04; payload length 2 ->
    // 02; payload de ad.
    const std::vector<uint8_t> expected = {0x00, 0x43, 0xE8, 0x09, 0x01,
                                           0x04, 0x02, 0xDE, 0xAD};
    CHECK(out == expected);
}

TEST_CASE("frame_encode appends to existing contents") {
    std::vector<uint8_t> out = {0xAA};
    REQUIRE(frame_encode(sample_video_frame(), out));
    CHECK(out.size() == 10);
    CHECK(out[0] == 0xAA);
    CHECK(out[1] == 0x00);
}

TEST_CASE("frame_encode rejects an empty payload") {
    Frame f = sample_video_frame();
    f.payload.clear();
    std::vector<uint8_t> out;
    CHECK_FALSE(frame_encode(f, out));
    CHECK(out.empty());
}

TEST_CASE("frame_encode rejects varint fields out of range") {
    Frame f = sample_video_frame();
    f.flow_id = kVarintMax + 1;
    std::vector<uint8_t> out;
    CHECK_FALSE(frame_encode(f, out));
    CHECK(out.empty());

    f = sample_video_frame();
    f.timestamp = kVarintMax + 1;
    CHECK_FALSE(frame_encode(f, out));
    CHECK(out.empty());
}

TEST_CASE("datagram_decode round-trips an encoded frame") {
    const Frame f = sample_video_frame();
    std::vector<uint8_t> wire;
    REQUIRE(frame_encode(f, wire));

    Frame out;
    REQUIRE(datagram_decode(wire, out) == DecodeStatus::Ok);
    CHECK(out == f);
}

TEST_CASE("datagram_decode rejects trailing bytes") {
    std::vector<uint8_t> wire;
    REQUIRE(frame_encode(sample_video_frame(), wire));
    wire.push_back(0x00);

    Frame out;
    CHECK(datagram_decode(wire, out) == DecodeStatus::Malformed);
}

TEST_CASE("datagram_decode rejects truncated input") {
    std::vector<uint8_t> wire;
    REQUIRE(frame_encode(sample_video_frame(), wire));

    Frame out;
    for (size_t len = 0; len < wire.size(); ++len) {
        CHECK(datagram_decode(std::span<const uint8_t>(wire.data(), len),
                              out) == DecodeStatus::Malformed);
    }
}

TEST_CASE("datagram_decode rejects zero payload length") {
    // flow 0, timestamp 0, type 9, msg stream 1, chunk stream 4,
    // payload length 0.
    const uint8_t wire[] = {0x00, 0x00, 0x09, 0x01, 0x04, 0x00};
    Frame out;
    CHECK(datagram_decode(wire, out) == DecodeStatus::Malformed);
}
