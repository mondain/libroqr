# libroqr Plan 1: Foundation and Sans-I/O Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Repository scaffolding plus the fully unit-tested sans-I/O RoQR protocol core (`roqr-core`): varint codec, frame encoder, datagram and incremental frame decoders, error codes, and flow table.

**Architecture:** `roqr-core` is a static C++20 library with zero I/O and zero picoquic types — byte-in/byte-out, per the approved spec (Approach A). Everything here is falsifiable in unit tests. Later plans add the picoquic transport (Plan 2), RTMP module (Plan 3), gateways/e2e (Plan 4), and FFI/JNI (Plan 5).

**Tech Stack:** C++20, CMake >= 3.24 with presets, Catch2 v3.7.1 (FetchContent), CTest.

**Spec:** `docs/superpowers/specs/2026-07-04-libroqr-design.md`. Draft: `../roqr/draft-gregoire-rtmp-over-quic.txt` (section references below are to this draft). E-RTMP references: `docs/reference/enhanced-rtmp-v1.md`, `docs/reference/enhanced-rtmp-v2.md`.

**E-RTMP note:** the spec requires Enhanced RTMP (Veovera v1/v2) support in the gateway media classifier. Nothing in this plan touches it — `roqr-core` treats RTMP payloads as opaque bytes by design — so E-RTMP work lands in Plan 3 (classifier) and Plan 4 (HEVC e2e case); see Follow-On Plans.

## Global Constraints

- C++20 (`CMAKE_CXX_STANDARD 20`, `REQUIRED ON`, extensions OFF); CMake floor 3.24.
- C++ namespace `roqr::`; library target names `roqr-core` etc.; include layout `core/include/roqr/*.hpp`.
- License Apache-2.0.
- Do not create any Markdown files (no README) in this plan; docs come later with explicit authorization.
- Commit messages: plain imperative, no emoji, no Claude tagline, no Co-Authored-By.
- Payload Length MUST be > 0 (draft s7.2). Datagram frames MUST be exactly one complete frame, no trailing bytes (draft s7.5). Varint max is 2^62-1 (RFC 9000 s16).
- Build/test commands used throughout: `cmake --preset dev`, `cmake --build --preset dev`, `ctest --preset dev`.

---

### Task 1: Repository scaffolding and build skeleton

**Files:**
- Create: `LICENSE` (Apache-2.0, fetched)
- Create: `.gitignore`
- Create: `CMakeLists.txt`
- Create: `CMakePresets.json`
- Create: `core/CMakeLists.txt`
- Create: `core/include/roqr/version.hpp`
- Create: `core/src/version.cpp`
- Create: `tests/CMakeLists.txt`
- Test: `tests/core/version_test.cpp`

**Interfaces:**
- Consumes: nothing (first task).
- Produces: `const char* roqr::version()` returning `"0.1.0"`; CMake targets `roqr-core` (static lib) and `roqr-core-tests` (Catch2, CTest-registered); presets `dev` and `release`. Later tasks append sources to `core/CMakeLists.txt` and test files to `tests/CMakeLists.txt`.

- [ ] **Step 1: Fetch the Apache-2.0 license text**

```bash
curl -fsSo LICENSE https://www.apache.org/licenses/LICENSE-2.0.txt
```

- [ ] **Step 2: Create `.gitignore`**

```gitignore
build/
.cache/
compile_commands.json
```

- [ ] **Step 3: Create root `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)

project(libroqr
  VERSION 0.1.0
  DESCRIPTION "RTMP over QUIC (RoQR) client library"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ROQR_BUILD_TESTS "Build unit tests" ON)

add_subdirectory(core)

if(ROQR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
```

- [ ] **Step 4: Create `CMakePresets.json`**

```json
{
  "version": 5,
  "configurePresets": [
    {
      "name": "dev",
      "binaryDir": "${sourceDir}/build/dev",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "ROQR_BUILD_TESTS": "ON",
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON"
      }
    },
    {
      "name": "release",
      "binaryDir": "${sourceDir}/build/release",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release"
      }
    }
  ],
  "buildPresets": [
    { "name": "dev", "configurePreset": "dev" },
    { "name": "release", "configurePreset": "release" }
  ],
  "testPresets": [
    {
      "name": "dev",
      "configurePreset": "dev",
      "output": { "outputOnFailure": true }
    }
  ]
}
```

- [ ] **Step 5: Create `core/CMakeLists.txt`**

```cmake
add_library(roqr-core STATIC
  src/version.cpp
)

target_include_directories(roqr-core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(roqr-core PUBLIC cxx_std_20)
```

- [ ] **Step 6: Create `core/include/roqr/version.hpp`**

```cpp
#pragma once

namespace roqr {

// Library version, "major.minor.patch". Keep in sync with the CMake
// project() version.
const char* version();

}  // namespace roqr
```

- [ ] **Step 7: Create `core/src/version.cpp`**

```cpp
#include "roqr/version.hpp"

namespace roqr {

const char* version() {
    return "0.1.0";
}

}  // namespace roqr
```

- [ ] **Step 8: Create `tests/CMakeLists.txt`**

```cmake
include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
)
FetchContent_MakeAvailable(Catch2)

add_executable(roqr-core-tests
  core/version_test.cpp
)

target_link_libraries(roqr-core-tests PRIVATE roqr-core Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(roqr-core-tests)
```

- [ ] **Step 9: Create `tests/core/version_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "roqr/version.hpp"

TEST_CASE("version string matches project version") {
    CHECK(std::strcmp(roqr::version(), "0.1.0") == 0);
}
```

- [ ] **Step 10: Configure, build, and run tests**

Run: `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`
Expected: configure succeeds, build succeeds, `100% tests passed, 0 tests failed out of 1`

- [ ] **Step 11: Commit**

```bash
git add LICENSE .gitignore CMakeLists.txt CMakePresets.json core tests
git commit -m "Add build skeleton with roqr-core stub and Catch2 tests"
```

---

### Task 2: Varint size and encode (RFC 9000 s16)

**Files:**
- Create: `core/include/roqr/varint.hpp`
- Create: `core/src/varint.cpp`
- Modify: `core/CMakeLists.txt` (add `src/varint.cpp`)
- Modify: `tests/CMakeLists.txt` (add `core/varint_test.cpp`)
- Test: `tests/core/varint_test.cpp`

**Interfaces:**
- Consumes: nothing from prior tasks (header-only usage of the build skeleton).
- Produces:
  - `roqr::kVarintMax` (`inline constexpr uint64_t`, value `(1ull << 62) - 1`)
  - `size_t roqr::varint_size(uint64_t value)` — 1, 2, 4, or 8; 0 if `value > kVarintMax`
  - `size_t roqr::varint_encode(uint64_t value, std::span<uint8_t> out)` — bytes written; 0 on out-of-range value or too-small buffer
  - `struct roqr::VarintDecode { uint64_t value; size_t consumed; }` and `std::optional<VarintDecode> roqr::varint_decode(std::span<const uint8_t> in)` are declared in this header but implemented in Task 3.

- [ ] **Step 1: Create `core/include/roqr/varint.hpp`**

```cpp
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
```

- [ ] **Step 2: Write the failing test `tests/core/varint_test.cpp`**

```cpp
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
```

Add the file to `tests/CMakeLists.txt`:

```cmake
add_executable(roqr-core-tests
  core/version_test.cpp
  core/varint_test.cpp
)
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to link (undefined reference to `roqr::varint_size` and `roqr::varint_encode`)

- [ ] **Step 4: Implement `core/src/varint.cpp`**

```cpp
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
    // Implemented in the next task.
    (void)in;
    return std::nullopt;
}

}  // namespace roqr
```

Add the file to `core/CMakeLists.txt`:

```cmake
add_library(roqr-core STATIC
  src/varint.cpp
  src/version.cpp
)
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS, `0 tests failed out of 4`

- [ ] **Step 6: Commit**

```bash
git add core tests
git commit -m "Add varint size and encode with RFC 9000 test vectors"
```

---

### Task 3: Varint decode

**Files:**
- Modify: `core/src/varint.cpp` (replace the stub `varint_decode`)
- Test: `tests/core/varint_test.cpp` (append cases)

**Interfaces:**
- Consumes: `varint_encode`, `varint_size`, `VarintDecode` from Task 2.
- Produces: working `std::optional<VarintDecode> roqr::varint_decode(std::span<const uint8_t>)` — nullopt means "need more bytes" (a varint prefix is never invalid, only incomplete). Task 4+ parse frame headers with it.

- [ ] **Step 1: Append failing tests to `tests/core/varint_test.cpp`**

```cpp
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
```

- [ ] **Step 2: Run the tests to verify the new ones fail**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: FAIL — the decode test cases fail (stub returns nullopt)

- [ ] **Step 3: Replace the `varint_decode` stub in `core/src/varint.cpp`**

```cpp
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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS, `0 tests failed out of 8`

- [ ] **Step 5: Commit**

```bash
git add core tests
git commit -m "Add varint decode"
```

---

### Task 4: Frame type and encoder (draft s7.2)

**Files:**
- Create: `core/include/roqr/frame.hpp`
- Create: `core/src/frame.cpp`
- Modify: `core/CMakeLists.txt` (add `src/frame.cpp`)
- Modify: `tests/CMakeLists.txt` (add `core/frame_test.cpp`)
- Test: `tests/core/frame_test.cpp`

**Interfaces:**
- Consumes: `varint_encode`, `varint_size`, `kVarintMax` from Task 2.
- Produces:
  - `struct roqr::Frame { uint64_t flow_id; uint64_t timestamp; uint8_t message_type; uint64_t message_stream_id; uint64_t chunk_stream_id; std::vector<uint8_t> payload; }` with defaulted `operator==`
  - `bool roqr::frame_encode(const Frame&, std::vector<uint8_t>& out)` — appends; false (out untouched) on empty payload or any varint field `> kVarintMax`
  - `enum class roqr::DecodeStatus { Ok, NeedMoreData, Malformed }` declared here; decoders in Tasks 5-6.

- [ ] **Step 1: Create `core/include/roqr/frame.hpp`**

```cpp
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "roqr/varint.hpp"

namespace roqr {

// One RoQR frame: RTMP message metadata plus exactly one complete RTMP
// message payload (draft s7.2).
struct Frame {
    uint64_t flow_id = 0;
    uint64_t timestamp = 0;
    uint8_t message_type = 0;
    uint64_t message_stream_id = 0;
    uint64_t chunk_stream_id = 0;
    std::vector<uint8_t> payload;

    bool operator==(const Frame&) const = default;
};

// Appends the encoded frame to out. Returns false and leaves out untouched
// if any varint field exceeds kVarintMax or the payload is empty (draft
// s7.2: Payload Length MUST be greater than zero).
bool frame_encode(const Frame& frame, std::vector<uint8_t>& out);

enum class DecodeStatus { Ok, NeedMoreData, Malformed };

// Decodes a DATAGRAM-carried frame: data MUST contain exactly one complete
// frame and no trailing bytes (draft s7.5). Incomplete or trailing input is
// Malformed, never NeedMoreData.
DecodeStatus datagram_decode(std::span<const uint8_t> data, Frame& out);

// Incremental decoder for stream-carried frames (draft s7.4). Feed stream
// bytes as they arrive; complete frames become available via next().
class FrameDecoder {
public:
    static constexpr uint64_t kDefaultMaxPayload = 16ull * 1024 * 1024;

    // max_payload bounds the accepted Payload Length; larger values mark
    // the decoder malformed (resource guard, draft s14).
    explicit FrameDecoder(uint64_t max_payload = kDefaultMaxPayload);

    void feed(std::span<const uint8_t> data);
    std::optional<Frame> next();
    bool malformed() const { return malformed_; }

private:
    void parse();

    std::vector<uint8_t> buffer_;
    std::deque<Frame> ready_;
    uint64_t max_payload_;
    bool malformed_ = false;
};

}  // namespace roqr
```

- [ ] **Step 2: Write the failing test `tests/core/frame_test.cpp`**

```cpp
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
```

Add the file to `tests/CMakeLists.txt`:

```cmake
add_executable(roqr-core-tests
  core/version_test.cpp
  core/varint_test.cpp
  core/frame_test.cpp
)
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to link (undefined reference to `roqr::frame_encode`)

- [ ] **Step 4: Implement `core/src/frame.cpp`**

```cpp
#include "roqr/frame.hpp"

namespace roqr {

namespace {

bool append_varint(uint64_t value, std::vector<uint8_t>& out) {
    uint8_t tmp[8];
    const size_t n = varint_encode(value, tmp);
    if (n == 0) return false;
    out.insert(out.end(), tmp, tmp + n);
    return true;
}

}  // namespace

bool frame_encode(const Frame& frame, std::vector<uint8_t>& out) {
    if (frame.payload.empty()) return false;

    const size_t start = out.size();
    const bool ok = append_varint(frame.flow_id, out) &&
                    append_varint(frame.timestamp, out) &&
                    (out.push_back(frame.message_type), true) &&
                    append_varint(frame.message_stream_id, out) &&
                    append_varint(frame.chunk_stream_id, out) &&
                    append_varint(frame.payload.size(), out);
    if (!ok) {
        out.resize(start);
        return false;
    }
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    return true;
}

DecodeStatus datagram_decode(std::span<const uint8_t> data, Frame& out) {
    // Implemented in the next task.
    (void)data;
    (void)out;
    return DecodeStatus::Malformed;
}

FrameDecoder::FrameDecoder(uint64_t max_payload) : max_payload_(max_payload) {}

void FrameDecoder::feed(std::span<const uint8_t> data) {
    // Implemented in Task 6.
    (void)data;
}

std::optional<Frame> FrameDecoder::next() {
    return std::nullopt;
}

void FrameDecoder::parse() {}

}  // namespace roqr
```

Add the file to `core/CMakeLists.txt`:

```cmake
add_library(roqr-core STATIC
  src/frame.cpp
  src/varint.cpp
  src/version.cpp
)
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS, `0 tests failed out of 12`

- [ ] **Step 6: Commit**

```bash
git add core tests
git commit -m "Add RoQR frame type and encoder"
```

---

### Task 5: Datagram decode (draft s7.5)

**Files:**
- Modify: `core/src/frame.cpp` (shared header parser + real `datagram_decode`)
- Test: `tests/core/frame_test.cpp` (append cases)

**Interfaces:**
- Consumes: `Frame`, `DecodeStatus`, `frame_encode` from Task 4; `varint_decode` from Task 3.
- Produces: working `DecodeStatus roqr::datagram_decode(std::span<const uint8_t>, Frame&)`; an internal (anonymous-namespace) `parse_header(std::span<const uint8_t>, Header&)` helper that Task 6 reuses. `Header` fields: `flow_id`, `timestamp`, `message_stream_id`, `chunk_stream_id`, `payload_length` (all `uint64_t`), `message_type` (`uint8_t`), `consumed` (`size_t`).

- [ ] **Step 1: Append failing tests to `tests/core/frame_test.cpp`**

```cpp
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
```

- [ ] **Step 2: Run to verify the new tests fail**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: FAIL — round-trip case fails (stub returns Malformed)

- [ ] **Step 3: Implement the header parser and `datagram_decode` in `core/src/frame.cpp`**

Add to the anonymous namespace (below `append_varint`):

```cpp
struct Header {
    uint64_t flow_id = 0;
    uint64_t timestamp = 0;
    uint64_t message_stream_id = 0;
    uint64_t chunk_stream_id = 0;
    uint64_t payload_length = 0;
    uint8_t message_type = 0;
    size_t consumed = 0;
};

// Parses the fixed frame header. Returns NeedMoreData when data is too
// short; a header by itself is never Malformed (any byte sequence is a
// valid varint prefix).
DecodeStatus parse_header(std::span<const uint8_t> data, Header& h) {
    size_t off = 0;
    auto take = [&](uint64_t& out) {
        auto r = varint_decode(data.subspan(off));
        if (!r) return false;
        out = r->value;
        off += r->consumed;
        return true;
    };

    if (!take(h.flow_id) || !take(h.timestamp)) return DecodeStatus::NeedMoreData;
    if (off >= data.size()) return DecodeStatus::NeedMoreData;
    h.message_type = data[off++];
    if (!take(h.message_stream_id) || !take(h.chunk_stream_id) ||
        !take(h.payload_length)) {
        return DecodeStatus::NeedMoreData;
    }
    h.consumed = off;
    return DecodeStatus::Ok;
}

void header_to_frame(const Header& h, Frame& f) {
    f.flow_id = h.flow_id;
    f.timestamp = h.timestamp;
    f.message_type = h.message_type;
    f.message_stream_id = h.message_stream_id;
    f.chunk_stream_id = h.chunk_stream_id;
}
```

Replace the `datagram_decode` stub:

```cpp
DecodeStatus datagram_decode(std::span<const uint8_t> data, Frame& out) {
    Header h;
    if (parse_header(data, h) != DecodeStatus::Ok) return DecodeStatus::Malformed;
    if (h.payload_length == 0) return DecodeStatus::Malformed;
    if (data.size() - h.consumed != h.payload_length) return DecodeStatus::Malformed;

    header_to_frame(h, out);
    out.payload.assign(data.begin() + h.consumed, data.end());
    return DecodeStatus::Ok;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS, `0 tests failed out of 16`

- [ ] **Step 5: Commit**

```bash
git add core tests
git commit -m "Add datagram frame decode"
```

---

### Task 6: Incremental stream decoder (draft s7.4)

**Files:**
- Modify: `core/src/frame.cpp` (implement `FrameDecoder::feed/next/parse`)
- Test: `tests/core/frame_test.cpp` (append cases)

**Interfaces:**
- Consumes: `FrameDecoder` declaration from Task 4; `parse_header`, `header_to_frame` from Task 5.
- Produces: working `FrameDecoder` — `feed()` accumulates stream bytes, `next()` pops complete frames in order, `malformed()` latches on zero or oversized Payload Length; after malformed, further `feed()` is ignored. Plan 2's QUIC stream receive path consumes exactly this API.

- [ ] **Step 1: Append failing tests to `tests/core/frame_test.cpp`**

```cpp
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
```

- [ ] **Step 2: Run to verify the new tests fail**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: FAIL — FrameDecoder cases fail (stubs do nothing)

- [ ] **Step 3: Implement the decoder in `core/src/frame.cpp`**

Replace the `FrameDecoder` stubs:

```cpp
void FrameDecoder::feed(std::span<const uint8_t> data) {
    if (malformed_) return;
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    parse();
}

std::optional<Frame> FrameDecoder::next() {
    if (ready_.empty()) return std::nullopt;
    Frame f = std::move(ready_.front());
    ready_.pop_front();
    return f;
}

void FrameDecoder::parse() {
    for (;;) {
        Header h;
        if (parse_header(buffer_, h) != DecodeStatus::Ok) return;
        if (h.payload_length == 0 || h.payload_length > max_payload_) {
            malformed_ = true;
            buffer_.clear();
            return;
        }
        if (buffer_.size() - h.consumed < h.payload_length) return;

        Frame f;
        header_to_frame(h, f);
        const auto payload_begin =
            buffer_.begin() + static_cast<ptrdiff_t>(h.consumed);
        const auto payload_end =
            payload_begin + static_cast<ptrdiff_t>(h.payload_length);
        f.payload.assign(payload_begin, payload_end);
        ready_.push_back(std::move(f));
        buffer_.erase(buffer_.begin(), payload_end);
    }
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS, `0 tests failed out of 21`

- [ ] **Step 5: Commit**

```bash
git add core tests
git commit -m "Add incremental stream frame decoder"
```

---

### Task 7: Error codes and flow lifecycle (draft Table 2, s5)

**Files:**
- Create: `core/include/roqr/error.hpp`
- Create: `core/src/error.cpp`
- Create: `core/include/roqr/flow_table.hpp`
- Create: `core/src/flow_table.cpp`
- Modify: `core/CMakeLists.txt` (add `src/error.cpp`, `src/flow_table.cpp`)
- Modify: `tests/CMakeLists.txt` (add `core/flow_table_test.cpp`)
- Test: `tests/core/flow_table_test.cpp`

**Interfaces:**
- Consumes: `Frame` from Task 4.
- Produces:
  - `enum class roqr::ErrorCode : uint64_t { NoError=0x00, GeneralError=0x01, InternalError=0x02, FrameEncodingError=0x03, StreamCreationError=0x04, FrameCancelled=0x05, UnknownFlowId=0x06, ExpectationUnmet=0x07 }` and `const char* roqr::to_string(ErrorCode)`
  - `enum class roqr::FlowState { Unknown, Active, Retired }`
  - `struct roqr::FlowTableLimits { size_t max_unknown_frames = 32; size_t max_unknown_octets = 256 * 1024; }`
  - `class roqr::FlowTable` with `bool activate(uint64_t)`, `void retire(uint64_t)`, `FlowState state(uint64_t) const`; unknown-flow buffering methods are stubbed here and implemented in Task 8: `enum class BufferResult { Buffered, LimitExceeded }`, `BufferResult buffer_unknown(Frame)`, `std::vector<Frame> take_buffered(uint64_t)`.

- [ ] **Step 1: Create `core/include/roqr/error.hpp`**

```cpp
#pragma once

#include <cstdint>

namespace roqr {

// RoQR application error codes (draft Table 2).
enum class ErrorCode : uint64_t {
    NoError = 0x00,
    GeneralError = 0x01,
    InternalError = 0x02,
    FrameEncodingError = 0x03,
    StreamCreationError = 0x04,
    FrameCancelled = 0x05,
    UnknownFlowId = 0x06,
    ExpectationUnmet = 0x07,
};

// Registry name for the code, e.g. "FRAME_ENCODING_ERROR".
const char* to_string(ErrorCode code);

}  // namespace roqr
```

- [ ] **Step 2: Create `core/include/roqr/flow_table.hpp`**

```cpp
#pragma once

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "roqr/frame.hpp"

namespace roqr {

enum class FlowState { Unknown, Active, Retired };

struct FlowTableLimits {
    size_t max_unknown_frames = 32;
    size_t max_unknown_octets = 256 * 1024;
};

// Tracks Flow ID lifecycle (draft s5). Flow 0, the default RTMP session
// flow, starts Active. Frames for flows that are not yet Active can be
// buffered within bounded limits; the transport decides whether overflow
// stops a stream (UNKNOWN_FLOW_ID) or drops datagrams.
class FlowTable {
public:
    explicit FlowTable(FlowTableLimits limits = {});

    // Binds a flow to application state. Returns false if the ID was
    // retired earlier: Flow IDs MUST NOT be reused for unrelated media
    // (draft s5), enforced here as never-reuse-within-a-connection.
    bool activate(uint64_t flow_id);
    void retire(uint64_t flow_id);
    FlowState state(uint64_t flow_id) const;

    enum class BufferResult { Buffered, LimitExceeded };

    // Buffers a frame for a not-yet-Active flow. Enforces both the frame
    // count and octet limits (draft s5); on LimitExceeded the frame is not
    // buffered.
    BufferResult buffer_unknown(Frame frame);

    // Drains and returns frames buffered for flow_id, preserving arrival
    // order. Called on activation.
    std::vector<Frame> take_buffered(uint64_t flow_id);

private:
    FlowTableLimits limits_;
    std::unordered_map<uint64_t, FlowState> states_;
    std::deque<Frame> unknown_;
    size_t unknown_octets_ = 0;
};

}  // namespace roqr
```

- [ ] **Step 3: Write the failing test `tests/core/flow_table_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "roqr/error.hpp"
#include "roqr/flow_table.hpp"

using namespace roqr;

TEST_CASE("error codes match draft Table 2") {
    CHECK(static_cast<uint64_t>(ErrorCode::NoError) == 0x00);
    CHECK(static_cast<uint64_t>(ErrorCode::GeneralError) == 0x01);
    CHECK(static_cast<uint64_t>(ErrorCode::InternalError) == 0x02);
    CHECK(static_cast<uint64_t>(ErrorCode::FrameEncodingError) == 0x03);
    CHECK(static_cast<uint64_t>(ErrorCode::StreamCreationError) == 0x04);
    CHECK(static_cast<uint64_t>(ErrorCode::FrameCancelled) == 0x05);
    CHECK(static_cast<uint64_t>(ErrorCode::UnknownFlowId) == 0x06);
    CHECK(static_cast<uint64_t>(ErrorCode::ExpectationUnmet) == 0x07);

    CHECK(std::strcmp(to_string(ErrorCode::FrameEncodingError),
                      "FRAME_ENCODING_ERROR") == 0);
    CHECK(std::strcmp(to_string(ErrorCode::UnknownFlowId),
                      "UNKNOWN_FLOW_ID") == 0);
}

TEST_CASE("flow 0 is the default session flow and starts Active") {
    FlowTable table;
    CHECK(table.state(0) == FlowState::Active);
}

TEST_CASE("unbound flows are Unknown until activated") {
    FlowTable table;
    CHECK(table.state(7) == FlowState::Unknown);
    CHECK(table.activate(7));
    CHECK(table.state(7) == FlowState::Active);
}

TEST_CASE("retired flows cannot be reactivated") {
    FlowTable table;
    REQUIRE(table.activate(7));
    table.retire(7);
    CHECK(table.state(7) == FlowState::Retired);
    CHECK_FALSE(table.activate(7));
    CHECK(table.state(7) == FlowState::Retired);
}

TEST_CASE("activating an already-active flow is idempotent") {
    FlowTable table;
    REQUIRE(table.activate(7));
    CHECK(table.activate(7));
    CHECK(table.state(7) == FlowState::Active);
}
```

Add the file to `tests/CMakeLists.txt`:

```cmake
add_executable(roqr-core-tests
  core/version_test.cpp
  core/varint_test.cpp
  core/frame_test.cpp
  core/flow_table_test.cpp
)
```

- [ ] **Step 4: Run to verify it fails**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to link (undefined references to `roqr::to_string`, `roqr::FlowTable::...`)

- [ ] **Step 5: Create `core/src/error.cpp`**

```cpp
#include "roqr/error.hpp"

namespace roqr {

const char* to_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::NoError: return "NO_ERROR";
        case ErrorCode::GeneralError: return "GENERAL_ERROR";
        case ErrorCode::InternalError: return "INTERNAL_ERROR";
        case ErrorCode::FrameEncodingError: return "FRAME_ENCODING_ERROR";
        case ErrorCode::StreamCreationError: return "STREAM_CREATION_ERROR";
        case ErrorCode::FrameCancelled: return "FRAME_CANCELLED";
        case ErrorCode::UnknownFlowId: return "UNKNOWN_FLOW_ID";
        case ErrorCode::ExpectationUnmet: return "EXPECTATION_UNMET";
    }
    return "UNKNOWN";
}

}  // namespace roqr
```

- [ ] **Step 6: Create `core/src/flow_table.cpp`**

```cpp
#include "roqr/flow_table.hpp"

namespace roqr {

FlowTable::FlowTable(FlowTableLimits limits) : limits_(limits) {
    // Draft s5: Flow ID 0 is the default RTMP session flow.
    states_[0] = FlowState::Active;
}

bool FlowTable::activate(uint64_t flow_id) {
    auto it = states_.find(flow_id);
    if (it != states_.end() && it->second == FlowState::Retired) return false;
    states_[flow_id] = FlowState::Active;
    return true;
}

void FlowTable::retire(uint64_t flow_id) {
    states_[flow_id] = FlowState::Retired;
    take_buffered(flow_id);  // drop anything still queued for the flow
}

FlowState FlowTable::state(uint64_t flow_id) const {
    auto it = states_.find(flow_id);
    return it == states_.end() ? FlowState::Unknown : it->second;
}

FlowTable::BufferResult FlowTable::buffer_unknown(Frame frame) {
    // Implemented in the next task.
    (void)frame;
    return BufferResult::LimitExceeded;
}

std::vector<Frame> FlowTable::take_buffered(uint64_t flow_id) {
    // Implemented in the next task.
    (void)flow_id;
    return {};
}

}  // namespace roqr
```

Add both files to `core/CMakeLists.txt`:

```cmake
add_library(roqr-core STATIC
  src/error.cpp
  src/flow_table.cpp
  src/frame.cpp
  src/varint.cpp
  src/version.cpp
)
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS, `0 tests failed out of 26`

- [ ] **Step 8: Commit**

```bash
git add core tests
git commit -m "Add error codes and flow lifecycle table"
```

---

### Task 8: Bounded unknown-flow buffering (draft s5)

**Files:**
- Modify: `core/src/flow_table.cpp` (implement `buffer_unknown`, `take_buffered`)
- Test: `tests/core/flow_table_test.cpp` (append cases)

**Interfaces:**
- Consumes: `FlowTable`, `FlowTableLimits`, `BufferResult` from Task 7; `Frame` from Task 4.
- Produces: working unknown-flow buffering. Plan 2's transport calls `buffer_unknown` when a frame arrives for a non-Active flow, maps `LimitExceeded` to STOP_SENDING with `ErrorCode::UnknownFlowId` (streams) or a drop (datagrams), and drains via `take_buffered` after `activate`.

- [ ] **Step 1: Append failing tests to `tests/core/flow_table_test.cpp`**

```cpp
namespace {

Frame frame_for_flow(uint64_t flow_id, size_t payload_size) {
    Frame f;
    f.flow_id = flow_id;
    f.message_type = 9;
    f.payload.assign(payload_size, 0xAB);
    return f;
}

}  // namespace

TEST_CASE("unknown-flow frames buffer and drain in order") {
    FlowTable table;
    CHECK(table.buffer_unknown(frame_for_flow(5, 10)) ==
          FlowTable::BufferResult::Buffered);
    Frame second = frame_for_flow(5, 20);
    second.timestamp = 99;
    CHECK(table.buffer_unknown(second) == FlowTable::BufferResult::Buffered);

    auto drained = table.take_buffered(5);
    REQUIRE(drained.size() == 2);
    CHECK(drained[0].payload.size() == 10);
    CHECK(drained[1].timestamp == 99);
    CHECK(table.take_buffered(5).empty());
}

TEST_CASE("take_buffered only drains the requested flow") {
    FlowTable table;
    REQUIRE(table.buffer_unknown(frame_for_flow(5, 10)) ==
            FlowTable::BufferResult::Buffered);
    REQUIRE(table.buffer_unknown(frame_for_flow(6, 10)) ==
            FlowTable::BufferResult::Buffered);

    CHECK(table.take_buffered(5).size() == 1);
    CHECK(table.take_buffered(6).size() == 1);
}

TEST_CASE("frame-count limit is enforced") {
    FlowTable table(FlowTableLimits{.max_unknown_frames = 2,
                                    .max_unknown_octets = 1024});
    CHECK(table.buffer_unknown(frame_for_flow(5, 1)) ==
          FlowTable::BufferResult::Buffered);
    CHECK(table.buffer_unknown(frame_for_flow(5, 1)) ==
          FlowTable::BufferResult::Buffered);
    CHECK(table.buffer_unknown(frame_for_flow(5, 1)) ==
          FlowTable::BufferResult::LimitExceeded);
}

TEST_CASE("octet limit is enforced and released on drain") {
    FlowTable table(FlowTableLimits{.max_unknown_frames = 100,
                                    .max_unknown_octets = 25});
    CHECK(table.buffer_unknown(frame_for_flow(5, 20)) ==
          FlowTable::BufferResult::Buffered);
    CHECK(table.buffer_unknown(frame_for_flow(6, 20)) ==
          FlowTable::BufferResult::LimitExceeded);

    CHECK(table.take_buffered(5).size() == 1);
    CHECK(table.buffer_unknown(frame_for_flow(6, 20)) ==
          FlowTable::BufferResult::Buffered);
}

TEST_CASE("retiring a flow drops its buffered frames") {
    FlowTable table(FlowTableLimits{.max_unknown_frames = 100,
                                    .max_unknown_octets = 25});
    REQUIRE(table.buffer_unknown(frame_for_flow(5, 20)) ==
            FlowTable::BufferResult::Buffered);
    table.retire(5);
    CHECK(table.take_buffered(5).empty());
    // The octet budget was released by retire().
    CHECK(table.buffer_unknown(frame_for_flow(6, 20)) ==
          FlowTable::BufferResult::Buffered);
}
```

- [ ] **Step 2: Run to verify the new tests fail**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: FAIL — buffering cases fail (stub always returns LimitExceeded)

- [ ] **Step 3: Implement buffering in `core/src/flow_table.cpp`**

Replace the two stubs:

```cpp
FlowTable::BufferResult FlowTable::buffer_unknown(Frame frame) {
    if (unknown_.size() >= limits_.max_unknown_frames ||
        unknown_octets_ + frame.payload.size() > limits_.max_unknown_octets) {
        return BufferResult::LimitExceeded;
    }
    unknown_octets_ += frame.payload.size();
    unknown_.push_back(std::move(frame));
    return BufferResult::Buffered;
}

std::vector<Frame> FlowTable::take_buffered(uint64_t flow_id) {
    std::vector<Frame> out;
    for (auto it = unknown_.begin(); it != unknown_.end();) {
        if (it->flow_id == flow_id) {
            unknown_octets_ -= it->payload.size();
            out.push_back(std::move(*it));
            it = unknown_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS, `0 tests failed out of 31`

- [ ] **Step 5: Commit**

```bash
git add core tests
git commit -m "Add bounded unknown-flow buffering"
```

---

## Completion Criteria

- `ctest --preset dev` passes with all 31 test cases green.
- `roqr-core` builds as a standalone static library with no dependencies beyond the C++ standard library.
- Draft coverage in core: s7.2 frame layout and payload rules, s7.4 incremental stream decoding, s7.5 exact-datagram rule, s5 flow lifecycle / no-reuse / bounded unknown-flow buffering, Table 2 error codes.

## Follow-On Plans (not in this document)

1. Plan 2: `roqr-quic` picoquic client transport plus `tools/roqr-relayd`, loopback integration tests.
2. Plan 3: `roqr-rtmp` handshake, chunking, AMF0, and the E-RTMP-aware media classifier (legacy FLV tag headers plus Enhanced RTMP v1/v2 ex-headers: IsExHeader bit, VideoPacketType/AudioPacketType incl. ModEx skip and Multitrack, FourCC extraction) with unit vectors per the spec.
3. Plan 4: gateway examples (`roqr-ingest`, `roqr-egress`, `roqr-duplex`), relayd command handling, ffmpeg end-to-end script including the E-RTMP HEVC case (auto-skipped without enhanced-FLV ffmpeg), GitHub Actions CI.
4. Plan 5: C FFI (`roqr.h`, `roqr_rtmp.h`), JNI bindings and Java samples.
