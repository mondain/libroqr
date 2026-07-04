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
