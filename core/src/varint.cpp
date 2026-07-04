#include "roqr/varint.hpp"

namespace roqr {

size_t varint_size(uint64_t value) {
    if (value <= 63) return 1;
    if (value <= 16383) return 2;
    if (value <= 1073741823) return 4;
    if (value <= kVarintMax) return 8;
    return 0;
}

size_t varint_encode(uint64_t value, std::span<uint8_t> out) {
    const size_t len = varint_size(value);
    if (len == 0 || out.size() < len) return 0;

    uint64_t v = value;
    for (size_t i = len; i-- > 0;) {
        out[i] = static_cast<uint8_t>(v & 0xFF);
        v >>= 8;
    }
    // Two-bit length prefix: 1 -> 00, 2 -> 01, 4 -> 10, 8 -> 11. The top
    // two bits of out[0] are zero because value fits the chosen length.
    const uint8_t prefix =
        len == 1 ? 0x00 : len == 2 ? 0x40 : len == 4 ? 0x80 : 0xC0;
    out[0] |= prefix;
    return len;
}

std::optional<VarintDecode> varint_decode(std::span<const uint8_t> in) {
    if (in.empty()) return std::nullopt;
    const size_t len = size_t{1} << (in[0] >> 6);
    if (in.size() < len) return std::nullopt;

    uint64_t value = in[0] & 0x3F;
    for (size_t i = 1; i < len; ++i) {
        value = (value << 8) | in[i];
    }
    return VarintDecode{value, len};
}

}  // namespace roqr
