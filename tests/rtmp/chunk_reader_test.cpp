#include <catch2/catch_test_macros.hpp>

#include "roqr/rtmp/chunk_reader.hpp"

using namespace roqr::rtmp;

namespace {

void put_u24(uint32_t v, std::vector<uint8_t>& out) {
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put_u32be(uint32_t v, std::vector<uint8_t>& out) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put_u32le(uint32_t v, std::vector<uint8_t>& out) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

// fmt0 header for csid < 64.
void put_fmt0(uint32_t csid, uint32_t ts, uint32_t len, uint8_t type,
              uint32_t msid, std::vector<uint8_t>& out) {
    out.push_back(static_cast<uint8_t>(0x00 | csid));
    put_u24(ts >= 0xFFFFFF ? 0xFFFFFF : ts, out);
    put_u24(len, out);
    out.push_back(type);
    put_u32le(msid, out);
    if (ts >= 0xFFFFFF) put_u32be(ts, out);
}

}  // namespace

TEST_CASE("single fmt0 message decodes") {
    std::vector<uint8_t> wire;
    put_fmt0(4, 1000, 3, 9, 1, wire);
    wire.insert(wire.end(), {0xAA, 0xBB, 0xCC});

    ChunkReader r;
    r.feed(wire);
    auto m = r.next();
    REQUIRE(m.has_value());
    CHECK(m->timestamp == 1000);
    CHECK(m->type == 9);
    CHECK(m->message_stream_id == 1);
    CHECK(m->chunk_stream_id == 4);
    CHECK(m->payload == std::vector<uint8_t>{0xAA, 0xBB, 0xCC});
    CHECK_FALSE(r.next().has_value());
    CHECK_FALSE(r.failed());
}

TEST_CASE("fmt1 fmt2 fmt3 deltas accumulate") {
    std::vector<uint8_t> wire;
    put_fmt0(4, 100, 1, 9, 1, wire);
    wire.push_back(0x01);
    // fmt1: delta 40, len 1, type 9
    wire.push_back(0x40 | 4);
    put_u24(40, wire);
    put_u24(1, wire);
    wire.push_back(9);
    wire.push_back(0x02);
    // fmt2: delta 5
    wire.push_back(0x80 | 4);
    put_u24(5, wire);
    wire.push_back(0x03);
    // fmt3: new message, re-applies delta 5
    wire.push_back(0xC0 | 4);
    wire.push_back(0x04);

    ChunkReader r;
    r.feed(wire);
    const uint32_t expected_ts[] = {100, 140, 145, 150};
    const uint8_t expected_payload[] = {0x01, 0x02, 0x03, 0x04};
    for (int i = 0; i < 4; ++i) {
        auto m = r.next();
        REQUIRE(m.has_value());
        CHECK(m->timestamp == expected_ts[i]);
        CHECK(m->payload == std::vector<uint8_t>{expected_payload[i]});
    }
}

TEST_CASE("multi-chunk message reassembles across fmt3 continuations") {
    // 300-byte payload at default chunk size 128 -> 3 chunks.
    std::vector<uint8_t> payload(300);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i);
    }
    std::vector<uint8_t> wire;
    put_fmt0(4, 50, 300, 9, 1, wire);
    wire.insert(wire.end(), payload.begin(), payload.begin() + 128);
    wire.push_back(0xC0 | 4);
    wire.insert(wire.end(), payload.begin() + 128, payload.begin() + 256);
    wire.push_back(0xC0 | 4);
    wire.insert(wire.end(), payload.begin() + 256, payload.end());

    // Feed at every split point to exercise incremental parsing.
    for (size_t split = 1; split < wire.size(); split += 37) {
        ChunkReader r;
        r.feed(std::span<const uint8_t>(wire.data(), split));
        r.feed(std::span<const uint8_t>(wire.data() + split,
                                        wire.size() - split));
        auto m = r.next();
        REQUIRE(m.has_value());
        CHECK(m->payload == payload);
        CHECK(m->timestamp == 50);
    }
}

TEST_CASE("set chunk size applies mid-stream and is surfaced") {
    std::vector<uint8_t> wire;
    // Set Chunk Size 256 on csid 2, msid 0.
    put_fmt0(2, 0, 4, 1, 0, wire);
    put_u32be(256, wire);
    // 200-byte message now fits in one chunk.
    std::vector<uint8_t> payload(200, 0x7E);
    put_fmt0(4, 10, 200, 9, 1, wire);
    wire.insert(wire.end(), payload.begin(), payload.end());

    ChunkReader r;
    r.feed(wire);
    auto ctrl = r.next();
    REQUIRE(ctrl.has_value());
    CHECK(ctrl->type == 1);
    CHECK(r.chunk_size() == 256);
    auto m = r.next();
    REQUIRE(m.has_value());
    CHECK(m->payload == payload);
}

TEST_CASE("extended timestamps parse on fmt0 and fmt3 continuations") {
    const uint32_t big_ts = 0x01000000;  // >= 0xFFFFFF
    std::vector<uint8_t> payload(130, 0x55);
    std::vector<uint8_t> wire;
    put_fmt0(4, big_ts, 130, 9, 1, wire);  // helper emits 0xFFFFFF + ext
    wire.insert(wire.end(), payload.begin(), payload.begin() + 128);
    wire.push_back(0xC0 | 4);
    put_u32be(big_ts, wire);  // spec: fmt3 continuation repeats ext ts
    wire.insert(wire.end(), payload.begin() + 128, payload.end());

    ChunkReader r;
    r.feed(wire);
    auto m = r.next();
    REQUIRE(m.has_value());
    CHECK(m->timestamp == big_ts);
    CHECK(m->payload == payload);
}

TEST_CASE("interleaved chunk streams and abort") {
    std::vector<uint8_t> wire;
    // csid 4: 200-byte message (2 chunks at size 128).
    std::vector<uint8_t> pa(200, 0xAA);
    put_fmt0(4, 1, 200, 9, 1, wire);
    wire.insert(wire.end(), pa.begin(), pa.begin() + 128);
    // csid 5 interleaves a complete small message.
    put_fmt0(5, 2, 2, 8, 1, wire);
    wire.insert(wire.end(), {0x11, 0x22});
    // csid 4 finishes.
    wire.push_back(0xC0 | 4);
    wire.insert(wire.end(), pa.begin() + 128, pa.end());
    // csid 6 starts a message then gets aborted.
    put_fmt0(6, 3, 200, 9, 1, wire);
    std::vector<uint8_t> junk(128, 0x00);
    wire.insert(wire.end(), junk.begin(), junk.end());
    put_fmt0(2, 0, 4, 2, 0, wire);  // Abort csid 6
    put_u32be(6, wire);

    ChunkReader r;
    r.feed(wire);
    auto m1 = r.next();  // csid 5 completes first
    REQUIRE(m1.has_value());
    CHECK(m1->chunk_stream_id == 5);
    auto m2 = r.next();
    REQUIRE(m2.has_value());
    CHECK(m2->chunk_stream_id == 4);
    CHECK(m2->payload == pa);
    auto m3 = r.next();  // the Abort message itself surfaces
    REQUIRE(m3.has_value());
    CHECK(m3->type == 2);
    CHECK_FALSE(r.next().has_value());  // csid 6's partial was dropped
    CHECK_FALSE(r.failed());
}

TEST_CASE("oversized message length and bad chunk size fail") {
    std::vector<uint8_t> wire;
    put_fmt0(4, 0, ChunkReader::kMaxMessageSize + 1, 9, 1, wire);
    ChunkReader r;
    r.feed(wire);
    CHECK(r.failed());

    std::vector<uint8_t> wire2;
    put_fmt0(2, 0, 4, 1, 0, wire2);
    put_u32be(0x80000001, wire2);  // top bit set
    ChunkReader r2;
    r2.feed(wire2);
    r2.next();
    CHECK(r2.failed());
}
