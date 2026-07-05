#include <catch2/catch_test_macros.hpp>

#include "roqr/rtmp/chunk_reader.hpp"
#include "roqr/rtmp/chunk_writer.hpp"

using namespace roqr::rtmp;

namespace {
RtmpMessage msg(uint32_t csid, uint32_t ts, uint8_t type, uint32_t msid,
                std::vector<uint8_t> payload) {
    RtmpMessage m;
    m.chunk_stream_id = csid;
    m.timestamp = ts;
    m.type = type;
    m.message_stream_id = msid;
    m.payload = std::move(payload);
    return m;
}

RtmpMessage round_trip(const RtmpMessage& m, ChunkWriter& w) {
    std::vector<uint8_t> wire;
    REQUIRE(w.write(m, wire));
    ChunkReader r;
    r.feed(wire);
    auto out = r.next();
    REQUIRE(out.has_value());
    REQUIRE_FALSE(r.failed());
    return *out;
}
}  // namespace

TEST_CASE("single-chunk message round-trips") {
    ChunkWriter w;
    const auto m = msg(4, 1000, 9, 1, {0xAA, 0xBB});
    CHECK(round_trip(m, w) == m);
}

TEST_CASE("multi-chunk message round-trips") {
    ChunkWriter w;
    std::vector<uint8_t> payload(1000);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i * 7);
    }
    const auto m = msg(4, 22, 9, 1, payload);
    CHECK(round_trip(m, w) == m);
}

TEST_CASE("extended timestamp round-trips across continuations") {
    ChunkWriter w;
    const auto m = msg(4, 0x01234567, 9, 1, std::vector<uint8_t>(300, 0x3C));
    CHECK(round_trip(m, w) == m);
}

TEST_CASE("set_chunk_size emits a control message and both sides switch") {
    ChunkWriter w;
    std::vector<uint8_t> wire;
    w.set_chunk_size(4096, wire);
    CHECK(w.chunk_size() == 4096);

    const auto m = msg(4, 5, 9, 1, std::vector<uint8_t>(3000, 0x42));
    REQUIRE(w.write(m, wire));

    ChunkReader r;
    r.feed(wire);
    auto ctrl = r.next();
    REQUIRE(ctrl.has_value());
    CHECK(ctrl->type == kTypeSetChunkSize);
    CHECK(r.chunk_size() == 4096);
    auto out = r.next();
    REQUIRE(out.has_value());
    CHECK(*out == m);
}

TEST_CASE("large csid uses the escape forms and rejects invalid ids") {
    ChunkWriter w;
    const auto two_byte = msg(300, 1, 9, 1, {0x01});
    CHECK(round_trip(two_byte, w) == two_byte);
    const auto three_byte = msg(40000, 1, 9, 1, {0x02});
    CHECK(round_trip(three_byte, w) == three_byte);

    std::vector<uint8_t> out;
    CHECK_FALSE(w.write(msg(1, 0, 9, 1, {0x00}), out));
    CHECK_FALSE(w.write(msg(65600, 0, 9, 1, {0x00}), out));
}

TEST_CASE("write rejects payloads that overflow the 24-bit length field") {
    ChunkWriter w;
    std::vector<uint8_t> out;
    CHECK_FALSE(w.write(msg(4, 0, 9, 1, std::vector<uint8_t>(0x1000000, 0)),
                        out));
    CHECK(out.empty());
}

TEST_CASE("zero chunk size is rejected") {
    ChunkWriter w;
    std::vector<uint8_t> out;
    w.set_chunk_size(0, out);
    CHECK(w.chunk_size() == kDefaultChunkSize);
    CHECK(out.empty());

    ChunkWriter w0(0);
    CHECK(w0.chunk_size() == kDefaultChunkSize);
}
