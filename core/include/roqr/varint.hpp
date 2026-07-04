#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace roqr {

// Largest value representable as a QUIC variable-length integer
// (RFC 9000 s16).
inline constexpr uint64_t kVarintMax = (1ull << 62) - 1;

// Number of bytes needed to encode value (1, 2, 4, or 8), or 0 if
// value > kVarintMax.
size_t varint_size(uint64_t value);

// Encodes value into out. Returns the number of bytes written, or 0 if the
// value is out of range or out is too small.
size_t varint_encode(uint64_t value, std::span<uint8_t> out);

struct VarintDecode {
    uint64_t value;
    size_t consumed;
};

// Decodes one varint from the front of in. Returns nullopt if in does not
// yet contain the complete encoding; callers feed more data and retry.
std::optional<VarintDecode> varint_decode(std::span<const uint8_t> in);

}  // namespace roqr
