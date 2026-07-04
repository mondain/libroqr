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

    f = sample_video_frame();
    f.message_stream_id = kVarintMax + 1;
    out = {0xAA};
    CHECK_FALSE(frame_encode(f, out));
    REQUIRE(out.size() == 1);
    CHECK(out[0] == 0xAA);

    f = sample_video_frame();
    f.chunk_stream_id = kVarintMax + 1;
    CHECK_FALSE(frame_encode(f, out));
    REQUIRE(out.size() == 1);
    CHECK(out[0] == 0xAA);
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

TEST_CASE("FrameDecoder decodes a whole frame fed at once") {
    std::vector<uint8_t> wire;
    REQUIRE(frame_encode(sample_video_frame(), wire));

    FrameDecoder dec;
    dec.feed(wire);
    auto f = dec.next();
    REQUIRE(f.has_value());
    CHECK(*f == sample_video_frame());
    CHECK_FALSE(dec.next().has_value());
    CHECK_FALSE(dec.malformed());
}

TEST_CASE("FrameDecoder handles every split point") {
    std::vector<uint8_t> wire;
    REQUIRE(frame_encode(sample_video_frame(), wire));

    for (size_t split = 1; split < wire.size(); ++split) {
        FrameDecoder dec;
        dec.feed(std::span<const uint8_t>(wire.data(), split));
        CHECK_FALSE(dec.next().has_value());
        dec.feed(std::span<const uint8_t>(wire.data() + split,
                                          wire.size() - split));
        auto f = dec.next();
        REQUIRE(f.has_value());
        CHECK(*f == sample_video_frame());
    }
}

TEST_CASE("FrameDecoder decodes back-to-back frames in order") {
    Frame a = sample_video_frame();
    Frame b = sample_video_frame();
    b.timestamp = 2000;
    b.payload = {0x01, 0x02, 0x03};

    std::vector<uint8_t> wire;
    REQUIRE(frame_encode(a, wire));
    REQUIRE(frame_encode(b, wire));

    FrameDecoder dec;
    dec.feed(wire);
    auto f1 = dec.next();
    auto f2 = dec.next();
    REQUIRE(f1.has_value());
    REQUIRE(f2.has_value());
    CHECK(*f1 == a);
    CHECK(*f2 == b);
    CHECK_FALSE(dec.next().has_value());
}

TEST_CASE("FrameDecoder marks zero payload length malformed") {
    const uint8_t wire[] = {0x00, 0x00, 0x09, 0x01, 0x04, 0x00};
    FrameDecoder dec;
    dec.feed(wire);
    CHECK(dec.malformed());
    CHECK_FALSE(dec.next().has_value());
}

TEST_CASE("FrameDecoder retains frames decoded before a malformed frame") {
    std::vector<uint8_t> wire;
    REQUIRE(frame_encode(sample_video_frame(), wire));
    // Zero payload length frame directly after a valid frame in one feed.
    const uint8_t bad[] = {0x00, 0x00, 0x09, 0x01, 0x04, 0x00};
    wire.insert(wire.end(), bad, bad + 6);

    FrameDecoder dec;
    dec.feed(wire);
    CHECK(dec.malformed());
    auto f = dec.next();
    REQUIRE(f.has_value());
    CHECK(*f == sample_video_frame());
    CHECK_FALSE(dec.next().has_value());
}

TEST_CASE("FrameDecoder handles interleaved feed and next across frames") {
    Frame a = sample_video_frame();
    Frame b = sample_video_frame();
    b.timestamp = 2000;
    b.payload = {0x01, 0x02, 0x03};

    std::vector<uint8_t> wa, wb;
    REQUIRE(frame_encode(a, wa));
    REQUIRE(frame_encode(b, wb));

    FrameDecoder dec;
    // Partial frame A.
    dec.feed(std::span<const uint8_t>(wa.data(), wa.size() - 1));
    CHECK_FALSE(dec.next().has_value());
    // Rest of A plus partial B.
    std::vector<uint8_t> chunk(wa.end() - 1, wa.end());
    chunk.insert(chunk.end(), wb.begin(), wb.begin() + 3);
    dec.feed(chunk);
    auto f1 = dec.next();
    REQUIRE(f1.has_value());
    CHECK(*f1 == a);
    CHECK_FALSE(dec.next().has_value());
    // Rest of B.
    dec.feed(std::span<const uint8_t>(wb.data() + 3, wb.size() - 3));
    auto f2 = dec.next();
    REQUIRE(f2.has_value());
    CHECK(*f2 == b);
}

TEST_CASE("FrameDecoder enforces the payload cap and latches") {
    Frame f = sample_video_frame();
    f.payload.assign(64, 0xAB);
    std::vector<uint8_t> wire;
    REQUIRE(frame_encode(f, wire));

    FrameDecoder dec(/*max_payload=*/32);
    dec.feed(wire);
    CHECK(dec.malformed());
    CHECK_FALSE(dec.next().has_value());

    // Further input is ignored once malformed.
    dec.feed(wire);
    CHECK_FALSE(dec.next().has_value());
}
