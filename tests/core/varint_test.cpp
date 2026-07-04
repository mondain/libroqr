#include <catch2/catch_test_macros.hpp>

#include "roqr/varint.hpp"

using namespace roqr;

TEST_CASE("varint_size boundaries") {
    CHECK(varint_size(0) == 1);
    CHECK(varint_size(63) == 1);
    CHECK(varint_size(64) == 2);
    CHECK(varint_size(16383) == 2);
    CHECK(varint_size(16384) == 4);
    CHECK(varint_size(1073741823) == 4);
    CHECK(varint_size(1073741824) == 8);
    CHECK(varint_size(kVarintMax) == 8);
    CHECK(varint_size(kVarintMax + 1) == 0);
}

TEST_CASE("varint_encode RFC 9000 appendix A vectors") {
    uint8_t buf[8];

    REQUIRE(varint_encode(37, buf) == 1);
    CHECK(buf[0] == 0x25);

    REQUIRE(varint_encode(15293, buf) == 2);
    CHECK(buf[0] == 0x7b);
    CHECK(buf[1] == 0xbd);

    REQUIRE(varint_encode(494878333, buf) == 4);
    CHECK(buf[0] == 0x9d);
    CHECK(buf[1] == 0x7f);
    CHECK(buf[2] == 0x3e);
    CHECK(buf[3] == 0x7d);

    REQUIRE(varint_encode(151288809941952652ull, buf) == 8);
    CHECK(buf[0] == 0xc2);
    CHECK(buf[1] == 0x19);
    CHECK(buf[2] == 0x7c);
    CHECK(buf[3] == 0x5e);
    CHECK(buf[4] == 0xff);
    CHECK(buf[5] == 0x14);
    CHECK(buf[6] == 0xe8);
    CHECK(buf[7] == 0x8c);
}

TEST_CASE("varint_encode rejects out-of-range values and short buffers") {
    uint8_t buf[8];
    CHECK(varint_encode(kVarintMax + 1, buf) == 0);

    uint8_t one[1];
    CHECK(varint_encode(64, one) == 0);  // needs 2 bytes
}

TEST_CASE("varint_decode RFC 9000 appendix A vectors") {
    const uint8_t one[] = {0x25};
    auto r = varint_decode(one);
    REQUIRE(r.has_value());
    CHECK(r->value == 37);
    CHECK(r->consumed == 1);

    const uint8_t two[] = {0x7b, 0xbd};
    r = varint_decode(two);
    REQUIRE(r.has_value());
    CHECK(r->value == 15293);
    CHECK(r->consumed == 2);

    const uint8_t four[] = {0x9d, 0x7f, 0x3e, 0x7d};
    r = varint_decode(four);
    REQUIRE(r.has_value());
    CHECK(r->value == 494878333);
    CHECK(r->consumed == 4);

    const uint8_t eight[] = {0xc2, 0x19, 0x7c, 0x5e, 0xff, 0x14, 0xe8, 0x8c};
    r = varint_decode(eight);
    REQUIRE(r.has_value());
    CHECK(r->value == 151288809941952652ull);
    CHECK(r->consumed == 8);
}

TEST_CASE("varint_decode reports incomplete input") {
    CHECK_FALSE(varint_decode({}).has_value());

    const uint8_t partial_two[] = {0x7b};
    CHECK_FALSE(varint_decode(partial_two).has_value());

    const uint8_t partial_eight[] = {0xc2, 0x19, 0x7c};
    CHECK_FALSE(varint_decode(partial_eight).has_value());
}

TEST_CASE("varint_decode ignores trailing bytes") {
    const uint8_t data[] = {0x25, 0xFF, 0xFF};
    auto r = varint_decode(data);
    REQUIRE(r.has_value());
    CHECK(r->value == 37);
    CHECK(r->consumed == 1);
}

TEST_CASE("varint round-trip at boundaries") {
    const uint64_t values[] = {0,       63,         64,         16383,
                               16384,   1073741823, 1073741824, kVarintMax};
    for (uint64_t v : values) {
        uint8_t buf[8];
        const size_t n = varint_encode(v, buf);
        REQUIRE(n > 0);
        auto r = varint_decode(std::span<const uint8_t>(buf, n));
        REQUIRE(r.has_value());
        CHECK(r->value == v);
        CHECK(r->consumed == n);
    }
}
