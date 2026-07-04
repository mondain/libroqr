# libroqr Plan 3: RTMP Module (Handshake, Chunking, AMF0, E-RTMP Classifier, ServerSession) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The `roqr-rtmp` module: AMF0 codec, RTMP simple handshake (both roles), chunk reader/writer with extended timestamps, an E-RTMP-aware media classifier, and a TCP `ServerSession`/`Listener` that accepts RTMP publishers/players — everything the Plan 4 gateways need on the RTMP side.

**Architecture:** Every protocol layer is sans-I/O (byte-in/byte-out, unit-tested without sockets): AMF0 (`Amf0Value`, encode/decode), handshake state machines (`HandshakeResponder`/`HandshakeInitiator`), `ChunkReader`/`ChunkWriter`, and the classifier. Only Task 8's `ServerSession`/`Listener` touch POSIX TCP (thread per connection, gateway-grade). The integration test drives a real loopback TCP publish using our own initiator + writer as the client.

**Tech Stack:** C++20, POSIX sockets (Task 8 only), Catch2 v3, CTest. No new external dependencies. E-RTMP reference specs vendored at `docs/reference/enhanced-rtmp-v1.md` / `enhanced-rtmp-v2.md`; legacy RTMP semantics per Adobe's RTMP spec; ffmpeg source is the interop authority when the specs are ambiguous.

**Spec:** `docs/superpowers/specs/2026-07-04-libroqr-design.md` (roqr-rtmp component). RoQR draft s7.3 defines the timestamp-resolution duty this module owns at the gateway boundary.

## Global Constraints

- C++20; namespace `roqr::rtmp::`; include layout `rtmp/include/roqr/rtmp/*.hpp`, sources `rtmp/src/*.cpp`; static lib target `roqr-rtmp`; tests target `roqr-rtmp-tests` in `tests/rtmp/`.
- CMake option `ROQR_BUILD_RTMP` defaults **ON** (per spec); the module has no picoquic dependency and must build with `ROQR_BUILD_QUIC=OFF`.
- Warning flags on the lib: `$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>`; builds stay warning-clean.
- **E-RTMP fidelity rule:** the classifier's enum values, ModEx size encoding, and multitrack layouts MUST be verified against the vendored `docs/reference/enhanced-rtmp-v2.md` (and v1) before implementing. If this plan's code or test vectors contradict the vendored spec, the spec wins — fix the code AND the vectors, and record the correction in your task report.
- RTMP wire facts used throughout (from Adobe's spec): chunk basic header fmt(2 bits)+csid(6 bits), csid escape 0 → 2-byte (csid=64+b1), csid escape 1 → 3-byte (csid=64+b1+b2*256); message headers 11/7/3/0 bytes for fmt 0-3; message stream id in fmt0 is LITTLE-endian; all other multi-byte fields big-endian; extended timestamp when the 3-byte field saturates at 0xFFFFFF; default chunk size 128; Set Chunk Size (type 1) is 4 bytes BE with the top bit zero; Abort (type 2) carries a u32 csid; handshake packets are 1536 bytes (time(4) zero(4) random(1528)), version byte 0x03.
- Integration tests (Task 8 only): loopback TCP, fixed ports 45570-45579, bounded waits, no hangs.
- Commit messages: plain imperative, no emoji, no Claude tagline, no Co-Authored-By. TDD per task.
- Build/test: `cmake --build --preset dev && ctest --preset dev`. Baseline at plan start: 63 tests green.

---

### Task 1: Module skeleton and AMF0 value type + encoder

**Files:**
- Create: `rtmp/CMakeLists.txt`
- Create: `rtmp/include/roqr/rtmp/amf0.hpp`
- Create: `rtmp/src/amf0.cpp`
- Modify: `CMakeLists.txt` (root: `ROQR_BUILD_RTMP` option + subdir)
- Modify: `tests/CMakeLists.txt` (new `roqr-rtmp-tests` target)
- Test: `tests/rtmp/amf0_encode_test.cpp`

**Interfaces:**
- Consumes: nothing from prior plans (standalone module).
- Produces:
  - `class roqr::rtmp::Amf0Value` with `enum class Type { Number, Boolean, String, Object, EcmaArray, StrictArray, Null, Undefined, Date }`; factories `number(double)`, `boolean(bool)`, `string(std::string)`, `object()`, `ecma_array()`, `strict_array()`, `null()`, `undefined()`, `date(double ms, int16_t tz = 0)`; accessors `type()`, `as_number()`, `as_boolean()`, `as_string()`, `date_tz()`; object API `property_count()`, `key_at(i)`, `value_at(i)`, `find(key)` (nullptr if absent), `set(key, value)` (returns `*this` for chaining, appends or replaces); array API `element_count()`, `element_at(i)`, `push(value)`; defaulted `operator==`.
  - `void roqr::rtmp::amf0_encode(const Amf0Value&, std::vector<uint8_t>& out)` (appends).
  - Declared here, implemented in Task 2: `std::optional<size_t> amf0_decode(std::span<const uint8_t>, Amf0Value& out)` and `std::optional<std::vector<Amf0Value>> amf0_decode_all(std::span<const uint8_t>)` (Task 1 ships stubs returning nullopt).
  - CMake target `roqr-rtmp`, tests target `roqr-rtmp-tests`.

- [ ] **Step 1: Create `rtmp/include/roqr/rtmp/amf0.hpp`**

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace roqr::rtmp {

// AMF0 value covering the types RTMP command traffic uses. Objects and
// ECMA arrays are ordered name/value sequences (parallel keys_/values_
// storage keeps the recursive type fully standard); strict arrays are
// value sequences stored in values_.
class Amf0Value {
public:
    enum class Type {
        Number,
        Boolean,
        String,
        Object,
        EcmaArray,
        StrictArray,
        Null,
        Undefined,
        Date,
    };

    Amf0Value() = default;  // Null

    static Amf0Value number(double v);
    static Amf0Value boolean(bool v);
    static Amf0Value string(std::string v);
    static Amf0Value object();
    static Amf0Value ecma_array();
    static Amf0Value strict_array();
    static Amf0Value null();
    static Amf0Value undefined();
    static Amf0Value date(double ms, int16_t tz = 0);

    Type type() const { return type_; }
    double as_number() const { return number_; }
    bool as_boolean() const { return boolean_; }
    const std::string& as_string() const { return string_; }
    int16_t date_tz() const { return tz_; }

    // Object / EcmaArray properties (ordered).
    size_t property_count() const { return keys_.size(); }
    const std::string& key_at(size_t i) const { return keys_[i]; }
    const Amf0Value& value_at(size_t i) const { return values_[i]; }
    const Amf0Value* find(const std::string& key) const;
    Amf0Value& set(std::string key, Amf0Value value);

    // StrictArray elements.
    size_t element_count() const { return values_.size(); }
    const Amf0Value& element_at(size_t i) const { return values_[i]; }
    Amf0Value& push(Amf0Value value);

    bool operator==(const Amf0Value&) const = default;

private:
    Type type_ = Type::Null;
    double number_ = 0;
    bool boolean_ = false;
    int16_t tz_ = 0;
    std::string string_;
    std::vector<std::string> keys_;
    std::vector<Amf0Value> values_;
};

// Appends the AMF0 encoding of value to out. Strings longer than 65535
// bytes encode as Long String (marker 0x0C).
void amf0_encode(const Amf0Value& value, std::vector<uint8_t>& out);

// Decodes one AMF0 value from the front of data. Returns the consumed
// byte count, or nullopt on truncated/malformed input (unknown markers,
// nesting deeper than 32 levels).
std::optional<size_t> amf0_decode(std::span<const uint8_t> data,
                                  Amf0Value& out);

// Decodes consecutive AMF0 values until data is exhausted (RTMP command
// payload form). Returns nullopt if any value fails to decode.
std::optional<std::vector<Amf0Value>> amf0_decode_all(
    std::span<const uint8_t> data);

}  // namespace roqr::rtmp
```

- [ ] **Step 2: Write the failing test `tests/rtmp/amf0_encode_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include "roqr/rtmp/amf0.hpp"

using namespace roqr::rtmp;

TEST_CASE("amf0 encodes scalars to known bytes") {
    std::vector<uint8_t> out;

    amf0_encode(Amf0Value::number(1.0), out);
    CHECK(out == std::vector<uint8_t>{0x00, 0x3F, 0xF0, 0, 0, 0, 0, 0, 0});

    out.clear();
    amf0_encode(Amf0Value::boolean(true), out);
    CHECK(out == std::vector<uint8_t>{0x01, 0x01});

    out.clear();
    amf0_encode(Amf0Value::string("connect"), out);
    CHECK(out == std::vector<uint8_t>{0x02, 0x00, 0x07, 'c', 'o', 'n', 'n',
                                      'e', 'c', 't'});

    out.clear();
    amf0_encode(Amf0Value::null(), out);
    CHECK(out == std::vector<uint8_t>{0x05});

    out.clear();
    amf0_encode(Amf0Value::undefined(), out);
    CHECK(out == std::vector<uint8_t>{0x06});
}

TEST_CASE("amf0 encodes an object with end marker") {
    Amf0Value obj = Amf0Value::object();
    obj.set("app", Amf0Value::string("live"));

    std::vector<uint8_t> out;
    amf0_encode(obj, out);
    const std::vector<uint8_t> expected = {
        0x03,                                     // object marker
        0x00, 0x03, 'a',  'p',  'p',              // property name
        0x02, 0x00, 0x04, 'l',  'i',  'v', 'e',   // string value
        0x00, 0x00, 0x09,                         // object end
    };
    CHECK(out == expected);
}

TEST_CASE("amf0 encodes ecma array with count and end marker") {
    Amf0Value arr = Amf0Value::ecma_array();
    arr.set("n", Amf0Value::number(2.0));

    std::vector<uint8_t> out;
    amf0_encode(arr, out);
    const std::vector<uint8_t> expected = {
        0x08, 0x00, 0x00, 0x00, 0x01,             // marker + count 1
        0x00, 0x01, 'n',                          // name
        0x00, 0x40, 0x00, 0, 0, 0, 0, 0, 0,       // number 2.0
        0x00, 0x00, 0x09,                         // end
    };
    CHECK(out == expected);
}

TEST_CASE("amf0 encodes strict array and date") {
    Amf0Value arr = Amf0Value::strict_array();
    arr.push(Amf0Value::number(1.0));
    arr.push(Amf0Value::boolean(false));

    std::vector<uint8_t> out;
    amf0_encode(arr, out);
    const std::vector<uint8_t> expected_arr = {
        0x0A, 0x00, 0x00, 0x00, 0x02,             // marker + count 2
        0x00, 0x3F, 0xF0, 0, 0, 0, 0, 0, 0,       // number 1.0
        0x01, 0x00,                               // boolean false
    };
    CHECK(out == expected_arr);

    out.clear();
    amf0_encode(Amf0Value::date(1.0, 0), out);
    const std::vector<uint8_t> expected_date = {
        0x0B, 0x3F, 0xF0, 0, 0, 0, 0, 0, 0, 0x00, 0x00,  // ms + tz 0
    };
    CHECK(out == expected_date);
}

TEST_CASE("set replaces an existing property") {
    Amf0Value obj = Amf0Value::object();
    obj.set("k", Amf0Value::number(1.0));
    obj.set("k", Amf0Value::number(2.0));
    REQUIRE(obj.property_count() == 1);
    CHECK(obj.value_at(0).as_number() == 2.0);
    REQUIRE(obj.find("k") != nullptr);
    CHECK(obj.find("missing") == nullptr);
}
```

- [ ] **Step 3: Wire the build**

`rtmp/CMakeLists.txt`:

```cmake
add_library(roqr-rtmp STATIC
  src/amf0.cpp
)

target_include_directories(roqr-rtmp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(roqr-rtmp PUBLIC cxx_std_20)
target_compile_options(roqr-rtmp PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
```

Root `CMakeLists.txt`: add below the existing options:

```cmake
option(ROQR_BUILD_RTMP "Build the RTMP/AMF gateway module" ON)
```

and below `add_subdirectory(core)`:

```cmake
if(ROQR_BUILD_RTMP)
  add_subdirectory(rtmp)
endif()
```

`tests/CMakeLists.txt`: append after the quic block:

```cmake
if(ROQR_BUILD_RTMP)
  add_executable(roqr-rtmp-tests
    rtmp/amf0_encode_test.cpp
  )
  target_link_libraries(roqr-rtmp-tests PRIVATE roqr-rtmp Catch2::Catch2WithMain)
  catch_discover_tests(roqr-rtmp-tests PROPERTIES TIMEOUT 60)
endif()
```

- [ ] **Step 4: Run to verify RED**

Run: `cmake --preset dev && cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to compile/link (missing `amf0.cpp` symbols).

- [ ] **Step 5: Implement `rtmp/src/amf0.cpp`**

```cpp
#include "roqr/rtmp/amf0.hpp"

#include <bit>

namespace roqr::rtmp {

namespace {

constexpr uint8_t kNumber = 0x00;
constexpr uint8_t kBoolean = 0x01;
constexpr uint8_t kString = 0x02;
constexpr uint8_t kObject = 0x03;
constexpr uint8_t kNull = 0x05;
constexpr uint8_t kUndefined = 0x06;
constexpr uint8_t kEcmaArray = 0x08;
constexpr uint8_t kObjectEnd = 0x09;
constexpr uint8_t kStrictArray = 0x0A;
constexpr uint8_t kDate = 0x0B;
constexpr uint8_t kLongString = 0x0C;

void put_u16(uint16_t v, std::vector<uint8_t>& out) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put_u32(uint32_t v, std::vector<uint8_t>& out) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put_f64(double d, std::vector<uint8_t>& out) {
    const uint64_t b = std::bit_cast<uint64_t>(d);
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>(b >> shift));
    }
}

void put_property_name(const std::string& name, std::vector<uint8_t>& out) {
    put_u16(static_cast<uint16_t>(name.size()), out);
    out.insert(out.end(), name.begin(), name.end());
}

void put_properties(const Amf0Value& v, std::vector<uint8_t>& out) {
    for (size_t i = 0; i < v.property_count(); ++i) {
        put_property_name(v.key_at(i), out);
        amf0_encode(v.value_at(i), out);
    }
    put_u16(0, out);
    out.push_back(kObjectEnd);
}

}  // namespace

Amf0Value Amf0Value::number(double v) {
    Amf0Value r;
    r.type_ = Type::Number;
    r.number_ = v;
    return r;
}

Amf0Value Amf0Value::boolean(bool v) {
    Amf0Value r;
    r.type_ = Type::Boolean;
    r.boolean_ = v;
    return r;
}

Amf0Value Amf0Value::string(std::string v) {
    Amf0Value r;
    r.type_ = Type::String;
    r.string_ = std::move(v);
    return r;
}

Amf0Value Amf0Value::object() {
    Amf0Value r;
    r.type_ = Type::Object;
    return r;
}

Amf0Value Amf0Value::ecma_array() {
    Amf0Value r;
    r.type_ = Type::EcmaArray;
    return r;
}

Amf0Value Amf0Value::strict_array() {
    Amf0Value r;
    r.type_ = Type::StrictArray;
    return r;
}

Amf0Value Amf0Value::null() { return Amf0Value{}; }

Amf0Value Amf0Value::undefined() {
    Amf0Value r;
    r.type_ = Type::Undefined;
    return r;
}

Amf0Value Amf0Value::date(double ms, int16_t tz) {
    Amf0Value r;
    r.type_ = Type::Date;
    r.number_ = ms;
    r.tz_ = tz;
    return r;
}

const Amf0Value* Amf0Value::find(const std::string& key) const {
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) return &values_[i];
    }
    return nullptr;
}

Amf0Value& Amf0Value::set(std::string key, Amf0Value value) {
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            values_[i] = std::move(value);
            return *this;
        }
    }
    keys_.push_back(std::move(key));
    values_.push_back(std::move(value));
    return *this;
}

Amf0Value& Amf0Value::push(Amf0Value value) {
    values_.push_back(std::move(value));
    return *this;
}

void amf0_encode(const Amf0Value& value, std::vector<uint8_t>& out) {
    switch (value.type()) {
        case Amf0Value::Type::Number:
            out.push_back(kNumber);
            put_f64(value.as_number(), out);
            break;
        case Amf0Value::Type::Boolean:
            out.push_back(kBoolean);
            out.push_back(value.as_boolean() ? 1 : 0);
            break;
        case Amf0Value::Type::String:
            if (value.as_string().size() > 0xFFFF) {
                out.push_back(kLongString);
                put_u32(static_cast<uint32_t>(value.as_string().size()), out);
            } else {
                out.push_back(kString);
                put_u16(static_cast<uint16_t>(value.as_string().size()), out);
            }
            out.insert(out.end(), value.as_string().begin(),
                       value.as_string().end());
            break;
        case Amf0Value::Type::Object:
            out.push_back(kObject);
            put_properties(value, out);
            break;
        case Amf0Value::Type::EcmaArray:
            out.push_back(kEcmaArray);
            put_u32(static_cast<uint32_t>(value.property_count()), out);
            put_properties(value, out);
            break;
        case Amf0Value::Type::StrictArray:
            out.push_back(kStrictArray);
            put_u32(static_cast<uint32_t>(value.element_count()), out);
            for (size_t i = 0; i < value.element_count(); ++i) {
                amf0_encode(value.element_at(i), out);
            }
            break;
        case Amf0Value::Type::Null:
            out.push_back(kNull);
            break;
        case Amf0Value::Type::Undefined:
            out.push_back(kUndefined);
            break;
        case Amf0Value::Type::Date:
            out.push_back(kDate);
            put_f64(value.as_number(), out);
            put_u16(static_cast<uint16_t>(value.date_tz()), out);
            break;
    }
}

std::optional<size_t> amf0_decode(std::span<const uint8_t> data,
                                  Amf0Value& out) {
    // Implemented in the next task.
    (void)data;
    (void)out;
    return std::nullopt;
}

std::optional<std::vector<Amf0Value>> amf0_decode_all(
    std::span<const uint8_t> data) {
    (void)data;
    return std::nullopt;
}

}  // namespace roqr::rtmp
```

- [ ] **Step 6: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 63 prior + 5 new = 68.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt rtmp tests
git commit -m "Add rtmp module skeleton with AMF0 value type and encoder"
```

---

### Task 2: AMF0 decoder

**Files:**
- Modify: `rtmp/src/amf0.cpp` (replace the two stubs)
- Modify: `tests/CMakeLists.txt` (add test file)
- Test: `tests/rtmp/amf0_decode_test.cpp`

**Interfaces:**
- Consumes: `Amf0Value`, `amf0_encode` from Task 1.
- Produces: working `amf0_decode` (one value, returns consumed count) and `amf0_decode_all` (command payloads). Decoder rules: ECMA array count is advisory — parse properties until the 0x0000/0x09 end marker (ffmpeg writes approximate counts); Long String (0x0C) decodes to `Type::String`; unknown markers, truncation, or nesting deeper than 32 → nullopt.

- [ ] **Step 1: Write the failing test `tests/rtmp/amf0_decode_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include "roqr/rtmp/amf0.hpp"

using namespace roqr::rtmp;

namespace {
Amf0Value round_trip(const Amf0Value& v, size_t* consumed = nullptr) {
    std::vector<uint8_t> wire;
    amf0_encode(v, wire);
    Amf0Value out;
    auto n = amf0_decode(wire, out);
    REQUIRE(n.has_value());
    if (consumed != nullptr) *consumed = *n;
    REQUIRE(*n == wire.size());
    return out;
}
}  // namespace

TEST_CASE("amf0 round-trips every type") {
    CHECK(round_trip(Amf0Value::number(3.5)) == Amf0Value::number(3.5));
    CHECK(round_trip(Amf0Value::boolean(true)) == Amf0Value::boolean(true));
    CHECK(round_trip(Amf0Value::string("publish")) ==
          Amf0Value::string("publish"));
    CHECK(round_trip(Amf0Value::null()) == Amf0Value::null());
    CHECK(round_trip(Amf0Value::undefined()) == Amf0Value::undefined());
    CHECK(round_trip(Amf0Value::date(123456.0, -300)) ==
          Amf0Value::date(123456.0, -300));

    Amf0Value obj = Amf0Value::object();
    obj.set("app", Amf0Value::string("live"))
        .set("tcUrl", Amf0Value::string("rtmp://h/live"));
    Amf0Value nested = Amf0Value::object();
    nested.set("inner", obj);
    CHECK(round_trip(nested) == nested);

    Amf0Value arr = Amf0Value::strict_array();
    arr.push(Amf0Value::number(1)).push(Amf0Value::string("x"));
    CHECK(round_trip(arr) == arr);
}

TEST_CASE("ecma array decode trusts the end marker over the count") {
    // Count says 99 but only one property precedes the end marker.
    const std::vector<uint8_t> wire = {
        0x08, 0x00, 0x00, 0x00, 0x63,             // count 99 (a lie)
        0x00, 0x01, 'n',  0x00, 0x40, 0x00, 0, 0, 0, 0, 0, 0,
        0x00, 0x00, 0x09,
    };
    Amf0Value out;
    auto n = amf0_decode(wire, out);
    REQUIRE(n.has_value());
    CHECK(*n == wire.size());
    REQUIRE(out.type() == Amf0Value::Type::EcmaArray);
    REQUIRE(out.property_count() == 1);
    CHECK(out.value_at(0).as_number() == 2.0);
}

TEST_CASE("long string decodes to String") {
    std::string big(70000, 'x');
    Amf0Value v = Amf0Value::string(big);
    CHECK(round_trip(v).as_string() == big);
}

TEST_CASE("truncated input returns nullopt at every prefix") {
    Amf0Value obj = Amf0Value::object();
    obj.set("k", Amf0Value::number(1.0));
    std::vector<uint8_t> wire;
    amf0_encode(obj, wire);

    Amf0Value out;
    for (size_t len = 0; len < wire.size(); ++len) {
        CHECK_FALSE(
            amf0_decode(std::span<const uint8_t>(wire.data(), len), out)
                .has_value());
    }
}

TEST_CASE("unknown marker and depth bomb are rejected") {
    const uint8_t unknown[] = {0x0F, 0x00};
    Amf0Value out;
    CHECK_FALSE(amf0_decode(unknown, out).has_value());

    // 40 nested objects each holding property "a" -> exceeds depth 32.
    std::vector<uint8_t> bomb;
    for (int i = 0; i < 40; ++i) {
        bomb.push_back(0x03);
        bomb.push_back(0x00);
        bomb.push_back(0x01);
        bomb.push_back('a');
    }
    CHECK_FALSE(amf0_decode(bomb, out).has_value());
}

TEST_CASE("decode_all parses a command payload sequence") {
    std::vector<uint8_t> wire;
    amf0_encode(Amf0Value::string("connect"), wire);
    amf0_encode(Amf0Value::number(1.0), wire);
    Amf0Value obj = Amf0Value::object();
    obj.set("app", Amf0Value::string("live"));
    amf0_encode(obj, wire);

    auto values = amf0_decode_all(wire);
    REQUIRE(values.has_value());
    REQUIRE(values->size() == 3);
    CHECK((*values)[0].as_string() == "connect");
    CHECK((*values)[1].as_number() == 1.0);
    REQUIRE((*values)[2].find("app") != nullptr);
    CHECK((*values)[2].find("app")->as_string() == "live");

    wire.push_back(0x0F);  // trailing garbage marker
    CHECK_FALSE(amf0_decode_all(wire).has_value());
}
```

Add `rtmp/amf0_decode_test.cpp` to the `roqr-rtmp-tests` sources in `tests/CMakeLists.txt`.

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev && ctest --preset dev 2>&1 | tail -5`
Expected: new decode cases FAIL (stubs return nullopt).

- [ ] **Step 3: Replace the stubs in `rtmp/src/amf0.cpp`**

```cpp
namespace {

constexpr int kMaxDepth = 32;

struct Reader {
    std::span<const uint8_t> data;
    size_t pos = 0;

    bool need(size_t n) const { return data.size() - pos >= n; }
    uint8_t u8() { return data[pos++]; }
    uint16_t u16() {
        const uint16_t v = static_cast<uint16_t>(data[pos] << 8 | data[pos + 1]);
        pos += 2;
        return v;
    }
    uint32_t u32() {
        const uint32_t v = static_cast<uint32_t>(data[pos]) << 24 |
                           static_cast<uint32_t>(data[pos + 1]) << 16 |
                           static_cast<uint32_t>(data[pos + 2]) << 8 |
                           static_cast<uint32_t>(data[pos + 3]);
        pos += 4;
        return v;
    }
    double f64() {
        uint64_t b = 0;
        for (int i = 0; i < 8; ++i) b = b << 8 | data[pos + i];
        pos += 8;
        return std::bit_cast<double>(b);
    }
    bool str(size_t len, std::string& out) {
        if (!need(len)) return false;
        out.assign(reinterpret_cast<const char*>(data.data() + pos), len);
        pos += len;
        return true;
    }
};

bool decode_value(Reader& r, Amf0Value& out, int depth);

// Parses name/value pairs into out until the 0x0000/0x09 end marker.
bool decode_properties(Reader& r, Amf0Value& out, int depth) {
    for (;;) {
        if (!r.need(2)) return false;
        const uint16_t name_len = r.u16();
        if (name_len == 0) {
            if (!r.need(1)) return false;
            return r.u8() == 0x09;
        }
        std::string name;
        if (!r.str(name_len, name)) return false;
        Amf0Value value;
        if (!decode_value(r, value, depth)) return false;
        out.set(std::move(name), std::move(value));
    }
}

bool decode_value(Reader& r, Amf0Value& out, int depth) {
    if (depth > kMaxDepth) return false;
    if (!r.need(1)) return false;
    const uint8_t marker = r.u8();
    switch (marker) {
        case kNumber:
            if (!r.need(8)) return false;
            out = Amf0Value::number(r.f64());
            return true;
        case kBoolean:
            if (!r.need(1)) return false;
            out = Amf0Value::boolean(r.u8() != 0);
            return true;
        case kString: {
            if (!r.need(2)) return false;
            const uint16_t len = r.u16();
            std::string s;
            if (!r.str(len, s)) return false;
            out = Amf0Value::string(std::move(s));
            return true;
        }
        case kLongString: {
            if (!r.need(4)) return false;
            const uint32_t len = r.u32();
            std::string s;
            if (!r.str(len, s)) return false;
            out = Amf0Value::string(std::move(s));
            return true;
        }
        case kObject:
            out = Amf0Value::object();
            return decode_properties(r, out, depth + 1);
        case kEcmaArray:
            // The count is advisory (ffmpeg writes approximations); the
            // end marker is authoritative.
            if (!r.need(4)) return false;
            r.u32();
            out = Amf0Value::ecma_array();
            return decode_properties(r, out, depth + 1);
        case kStrictArray: {
            if (!r.need(4)) return false;
            const uint32_t count = r.u32();
            out = Amf0Value::strict_array();
            for (uint32_t i = 0; i < count; ++i) {
                Amf0Value v;
                if (!decode_value(r, v, depth + 1)) return false;
                out.push(std::move(v));
            }
            return true;
        }
        case kNull:
            out = Amf0Value::null();
            return true;
        case kUndefined:
            out = Amf0Value::undefined();
            return true;
        case kDate: {
            if (!r.need(10)) return false;
            const double ms = r.f64();
            const auto tz = static_cast<int16_t>(r.u16());
            out = Amf0Value::date(ms, tz);
            return true;
        }
        default:
            return false;
    }
}

}  // namespace

std::optional<size_t> amf0_decode(std::span<const uint8_t> data,
                                  Amf0Value& out) {
    Reader r{data};
    if (!decode_value(r, out, 0)) return std::nullopt;
    return r.pos;
}

std::optional<std::vector<Amf0Value>> amf0_decode_all(
    std::span<const uint8_t> data) {
    std::vector<Amf0Value> values;
    size_t pos = 0;
    while (pos < data.size()) {
        Amf0Value v;
        auto n = amf0_decode(data.subspan(pos), v);
        if (!n) return std::nullopt;
        pos += *n;
        values.push_back(std::move(v));
    }
    return values;
}
```

(The anonymous-namespace helpers go inside the existing anonymous namespace; the two public functions replace the stubs at the bottom of the file.)

- [ ] **Step 4: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 68 prior + 6 new = 74.

- [ ] **Step 5: Commit**

```bash
git add rtmp tests
git commit -m "Add AMF0 decoder with advisory ecma count and depth guard"
```

---

### Task 3: RTMP simple handshake (both roles)

**Files:**
- Create: `rtmp/include/roqr/rtmp/handshake.hpp`
- Create: `rtmp/src/handshake.cpp`
- Modify: `rtmp/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/rtmp/handshake_test.cpp`

**Interfaces:**
- Consumes: nothing from prior tasks.
- Produces:

```cpp
namespace roqr::rtmp {
inline constexpr size_t kHandshakePacketSize = 1536;
inline constexpr uint8_t kRtmpVersion = 3;

// Server side: consume C0/C1/C2, produce S0/S1/S2.
class HandshakeResponder {
public:
    // Appends any bytes to send to out. Returns false on protocol failure
    // (wrong version, C2 does not echo S1's random bytes).
    bool feed(std::span<const uint8_t> in, std::vector<uint8_t>& out);
    bool done() const;
};

// Client side: produce C0/C1, consume S0/S1/S2, produce C2.
class HandshakeInitiator {
public:
    std::vector<uint8_t> start();  // C0 + C1
    bool feed(std::span<const uint8_t> in, std::vector<uint8_t>& out);
    bool done() const;
};
}
```

  Both tolerate arbitrarily fragmented input (internal buffering). Packets: C1/S1 = time(4 BE) + zero(4) + 1528 random bytes; C2/S2 = peer's time + local read time + echo of the peer's random bytes. Verification: responder checks C2 bytes 8..1535 equal S1's; initiator checks S2 bytes 8..1535 equal C1's.

- [ ] **Step 1: Write the failing test `tests/rtmp/handshake_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include "roqr/rtmp/handshake.hpp"

using namespace roqr::rtmp;

TEST_CASE("initiator and responder complete a loopback handshake") {
    HandshakeInitiator client;
    HandshakeResponder server;

    std::vector<uint8_t> to_server = client.start();
    REQUIRE(to_server.size() == 1 + kHandshakePacketSize);
    CHECK(to_server[0] == kRtmpVersion);

    std::vector<uint8_t> to_client;
    REQUIRE(server.feed(to_server, to_client));
    REQUIRE(to_client.size() == 1 + 2 * kHandshakePacketSize);  // S0 S1 S2

    std::vector<uint8_t> c2;
    REQUIRE(client.feed(to_client, c2));
    REQUIRE(c2.size() == kHandshakePacketSize);
    CHECK(client.done());

    std::vector<uint8_t> none;
    REQUIRE(server.feed(c2, none));
    CHECK(none.empty());
    CHECK(server.done());
}

TEST_CASE("handshake survives byte-at-a-time delivery") {
    HandshakeInitiator client;
    HandshakeResponder server;

    std::vector<uint8_t> to_server = client.start();
    std::vector<uint8_t> to_client;
    for (uint8_t b : to_server) {
        REQUIRE(server.feed(std::span<const uint8_t>(&b, 1), to_client));
    }
    std::vector<uint8_t> c2;
    for (uint8_t b : to_client) {
        REQUIRE(client.feed(std::span<const uint8_t>(&b, 1), c2));
    }
    std::vector<uint8_t> none;
    for (uint8_t b : c2) {
        REQUIRE(server.feed(std::span<const uint8_t>(&b, 1), none));
    }
    CHECK(client.done());
    CHECK(server.done());
}

TEST_CASE("responder rejects a wrong version byte") {
    HandshakeResponder server;
    const uint8_t bad[] = {0x06};
    std::vector<uint8_t> out;
    CHECK_FALSE(server.feed(bad, out));
}

TEST_CASE("responder rejects a C2 that does not echo S1") {
    HandshakeInitiator client;
    HandshakeResponder server;
    std::vector<uint8_t> to_client;
    REQUIRE(server.feed(client.start(), to_client));

    std::vector<uint8_t> forged(kHandshakePacketSize, 0xEE);
    std::vector<uint8_t> none;
    CHECK_FALSE(server.feed(forged, none));
}
```

Add `rtmp/handshake_test.cpp` to `roqr-rtmp-tests`; add `src/handshake.cpp` to `roqr-rtmp`.

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to compile (missing header).

- [ ] **Step 3: Implement**

`rtmp/include/roqr/rtmp/handshake.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace roqr::rtmp {

inline constexpr size_t kHandshakePacketSize = 1536;
inline constexpr uint8_t kRtmpVersion = 3;

// RTMP simple (unencrypted) handshake, sans-I/O. Both classes buffer
// fragmented input internally; feed() appends any bytes that must be sent
// to the peer and returns false on protocol failure (latched).

class HandshakeResponder {
public:
    bool feed(std::span<const uint8_t> in, std::vector<uint8_t>& out);
    bool done() const { return state_ == State::Done; }

private:
    enum class State { WaitC0C1, WaitC2, Done, Failed };
    State state_ = State::WaitC0C1;
    std::vector<uint8_t> buffer_;
    std::vector<uint8_t> s1_;
};

class HandshakeInitiator {
public:
    std::vector<uint8_t> start();
    bool feed(std::span<const uint8_t> in, std::vector<uint8_t>& out);
    bool done() const { return state_ == State::Done; }

private:
    enum class State { Idle, WaitS0S1S2, Done, Failed };
    State state_ = State::Idle;
    std::vector<uint8_t> buffer_;
    std::vector<uint8_t> c1_;
};

}  // namespace roqr::rtmp
```

`rtmp/src/handshake.cpp`:

```cpp
#include "roqr/rtmp/handshake.hpp"

#include <algorithm>
#include <random>

namespace roqr::rtmp {

namespace {

std::vector<uint8_t> make_packet() {
    std::vector<uint8_t> pkt(kHandshakePacketSize, 0);
    // time(4) and zero(4) stay 0; fill the 1528 random bytes.
    std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 8; i < pkt.size(); ++i) {
        pkt[i] = static_cast<uint8_t>(dist(rng));
    }
    return pkt;
}

// C2/S2: peer time(4) + local time(4, we send 0) + peer random echo.
std::vector<uint8_t> make_echo(std::span<const uint8_t> peer_packet) {
    std::vector<uint8_t> echo(peer_packet.begin(), peer_packet.end());
    std::fill(echo.begin() + 4, echo.begin() + 8, 0);
    return echo;
}

bool random_matches(std::span<const uint8_t> echo,
                    std::span<const uint8_t> original) {
    return std::equal(echo.begin() + 8, echo.end(), original.begin() + 8);
}

}  // namespace

bool HandshakeResponder::feed(std::span<const uint8_t> in,
                              std::vector<uint8_t>& out) {
    if (state_ == State::Failed) return false;
    buffer_.insert(buffer_.end(), in.begin(), in.end());

    if (state_ == State::WaitC0C1) {
        if (buffer_.empty()) return true;
        if (buffer_[0] != kRtmpVersion) {
            state_ = State::Failed;
            return false;
        }
        if (buffer_.size() < 1 + kHandshakePacketSize) return true;
        const std::span<const uint8_t> c1(buffer_.data() + 1,
                                          kHandshakePacketSize);
        s1_ = make_packet();
        out.push_back(kRtmpVersion);                    // S0
        out.insert(out.end(), s1_.begin(), s1_.end());  // S1
        const auto s2 = make_echo(c1);                  // S2
        out.insert(out.end(), s2.begin(), s2.end());
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + 1 + kHandshakePacketSize);
        state_ = State::WaitC2;
    }
    if (state_ == State::WaitC2) {
        if (buffer_.size() < kHandshakePacketSize) return true;
        const std::span<const uint8_t> c2(buffer_.data(),
                                          kHandshakePacketSize);
        if (!random_matches(c2, s1_)) {
            state_ = State::Failed;
            return false;
        }
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + kHandshakePacketSize);
        state_ = State::Done;
    }
    return true;
}

std::vector<uint8_t> HandshakeInitiator::start() {
    c1_ = make_packet();
    std::vector<uint8_t> out;
    out.push_back(kRtmpVersion);
    out.insert(out.end(), c1_.begin(), c1_.end());
    state_ = State::WaitS0S1S2;
    return out;
}

bool HandshakeInitiator::feed(std::span<const uint8_t> in,
                              std::vector<uint8_t>& out) {
    if (state_ == State::Failed) return false;
    if (state_ != State::WaitS0S1S2) return state_ == State::Done;
    buffer_.insert(buffer_.end(), in.begin(), in.end());

    if (!buffer_.empty() && buffer_[0] != kRtmpVersion) {
        state_ = State::Failed;
        return false;
    }
    if (buffer_.size() < 1 + 2 * kHandshakePacketSize) return true;

    const std::span<const uint8_t> s1(buffer_.data() + 1,
                                      kHandshakePacketSize);
    const std::span<const uint8_t> s2(
        buffer_.data() + 1 + kHandshakePacketSize, kHandshakePacketSize);
    if (!random_matches(s2, c1_)) {
        state_ = State::Failed;
        return false;
    }
    const auto c2 = make_echo(s1);
    out.insert(out.end(), c2.begin(), c2.end());
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + 1 + 2 * kHandshakePacketSize);
    state_ = State::Done;
    return true;
}

}  // namespace roqr::rtmp
```

- [ ] **Step 4: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 74 prior + 4 new = 78.

- [ ] **Step 5: Commit**

```bash
git add rtmp tests
git commit -m "Add RTMP simple handshake for both roles"
```

---

### Task 4: Chunk reader (dechunker)

**Files:**
- Create: `rtmp/include/roqr/rtmp/message.hpp`
- Create: `rtmp/include/roqr/rtmp/chunk_reader.hpp`
- Create: `rtmp/src/chunk_reader.cpp`
- Modify: `rtmp/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/rtmp/chunk_reader_test.cpp`

**Interfaces:**
- Consumes: nothing from prior tasks.
- Produces:

```cpp
namespace roqr::rtmp {
struct RtmpMessage {
    uint32_t timestamp = 0;
    uint8_t type = 0;
    uint32_t message_stream_id = 0;
    uint32_t chunk_stream_id = 0;
    std::vector<uint8_t> payload;
    bool operator==(const RtmpMessage&) const = default;
};
inline constexpr uint32_t kDefaultChunkSize = 128;

class ChunkReader {
public:
    static constexpr uint32_t kMaxMessageSize = 8 * 1024 * 1024;  // must be < 0xFFFFFF+1 so a 24-bit length can exceed it
    void feed(std::span<const uint8_t> data);
    std::optional<RtmpMessage> next();
    bool failed() const;
    uint32_t chunk_size() const;  // tracks incoming Set Chunk Size
};
}
```

  Behavior: fmt 0-3 headers per the Global Constraints wire facts; extended timestamps (field saturates at 0xFFFFFF → 4-byte value follows, and fmt3 chunks read it again while the chunk stream's `extended` flag is set — ffmpeg writes it on continuations per spec); fmt1/2 deltas accumulate; a fmt3 chunk with no in-progress message starts a new message re-applying the stored delta; a fmt3 with no prior header state fails; Set Chunk Size (type 1) is applied internally AND surfaced; Abort (type 2) drops that csid's partial assembly and is surfaced; message length > kMaxMessageSize, chunk size with the top bit set, or chunk size 0 → `failed()` latches.

- [ ] **Step 1: Write the failing test `tests/rtmp/chunk_reader_test.cpp`**

```cpp
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
```

Add the test file and `src/chunk_reader.cpp` to the build.

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to compile (missing headers).

- [ ] **Step 3: Implement**

`rtmp/include/roqr/rtmp/message.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <vector>

namespace roqr::rtmp {

// One de-chunked RTMP message (draft s7.3: extended timestamps already
// resolved into the timestamp field).
struct RtmpMessage {
    uint32_t timestamp = 0;
    uint8_t type = 0;
    uint32_t message_stream_id = 0;
    uint32_t chunk_stream_id = 0;
    std::vector<uint8_t> payload;

    bool operator==(const RtmpMessage&) const = default;
};

inline constexpr uint32_t kDefaultChunkSize = 128;

// RTMP message type ids used by this module.
inline constexpr uint8_t kTypeSetChunkSize = 1;
inline constexpr uint8_t kTypeAbort = 2;
inline constexpr uint8_t kTypeAcknowledgement = 3;
inline constexpr uint8_t kTypeUserControl = 4;
inline constexpr uint8_t kTypeWindowAckSize = 5;
inline constexpr uint8_t kTypeSetPeerBandwidth = 6;
inline constexpr uint8_t kTypeAudio = 8;
inline constexpr uint8_t kTypeVideo = 9;
inline constexpr uint8_t kTypeDataAmf0 = 18;
inline constexpr uint8_t kTypeCommandAmf0 = 20;

}  // namespace roqr::rtmp
```

`rtmp/include/roqr/rtmp/chunk_reader.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "roqr/rtmp/message.hpp"

namespace roqr::rtmp {

// Incremental RTMP dechunker. Feed raw TCP bytes; complete messages come
// out of next() in wire order. Set Chunk Size and Abort are applied
// internally and also surfaced as messages.
class ChunkReader {
public:
    static constexpr uint32_t kMaxMessageSize = 8 * 1024 * 1024;  // must be < 0xFFFFFF+1 so a 24-bit length can exceed it

    void feed(std::span<const uint8_t> data);
    std::optional<RtmpMessage> next();
    bool failed() const { return failed_; }
    uint32_t chunk_size() const { return chunk_size_; }

private:
    struct CsidState {
        uint32_t timestamp = 0;
        uint32_t delta = 0;
        uint32_t length = 0;
        uint32_t message_stream_id = 0;
        uint8_t type = 0;
        bool extended = false;
        bool have_header = false;
        std::vector<uint8_t> assembling;
        uint32_t remaining = 0;
    };

    void parse();
    void finalize(uint32_t csid, CsidState& st);

    std::vector<uint8_t> buffer_;
    std::deque<RtmpMessage> ready_;
    std::map<uint32_t, CsidState> streams_;
    uint32_t chunk_size_ = kDefaultChunkSize;
    bool failed_ = false;
};

}  // namespace roqr::rtmp
```

`rtmp/src/chunk_reader.cpp`:

```cpp
#include "roqr/rtmp/chunk_reader.hpp"

namespace roqr::rtmp {

namespace {
uint32_t be24(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 16 |
           static_cast<uint32_t>(p[1]) << 8 | p[2];
}
uint32_t be32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 |
           static_cast<uint32_t>(p[1]) << 16 |
           static_cast<uint32_t>(p[2]) << 8 | p[3];
}
uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[3]) << 24 |
           static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[1]) << 8 | p[0];
}
}  // namespace

void ChunkReader::feed(std::span<const uint8_t> data) {
    if (failed_) return;
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    parse();
}

std::optional<RtmpMessage> ChunkReader::next() {
    if (ready_.empty()) return std::nullopt;
    RtmpMessage m = std::move(ready_.front());
    ready_.pop_front();
    return m;
}

void ChunkReader::finalize(uint32_t csid, CsidState& st) {
    RtmpMessage m;
    m.timestamp = st.timestamp;
    m.type = st.type;
    m.message_stream_id = st.message_stream_id;
    m.chunk_stream_id = csid;
    m.payload = std::move(st.assembling);
    st.assembling.clear();

    if (m.type == kTypeSetChunkSize && m.payload.size() >= 4) {
        const uint32_t size = be32(m.payload.data());
        if ((size & 0x80000000u) != 0 || size == 0) {
            failed_ = true;
            return;
        }
        chunk_size_ = size;
    } else if (m.type == kTypeAbort && m.payload.size() >= 4) {
        const uint32_t target = be32(m.payload.data());
        auto it = streams_.find(target);
        if (it != streams_.end()) {
            it->second.assembling.clear();
            it->second.remaining = 0;
        }
    }
    ready_.push_back(std::move(m));
}

void ChunkReader::parse() {
    for (;;) {
        if (failed_ || buffer_.empty()) return;

        size_t pos = 0;
        const uint8_t b0 = buffer_[0];
        const uint8_t fmt = b0 >> 6;
        uint32_t csid = b0 & 0x3F;
        pos = 1;
        if (csid == 0) {
            if (buffer_.size() < 2) return;
            csid = 64 + buffer_[1];
            pos = 2;
        } else if (csid == 1) {
            if (buffer_.size() < 3) return;
            csid = 64 + buffer_[1] + static_cast<uint32_t>(buffer_[2]) * 256;
            pos = 3;
        }

        const size_t mh_len = fmt == 0 ? 11 : fmt == 1 ? 7 : fmt == 2 ? 3 : 0;
        if (buffer_.size() < pos + mh_len) return;

        CsidState& st = streams_[csid];
        if (fmt == 3 && !st.have_header) {
            failed_ = true;
            return;
        }

        const bool starting_new = st.remaining == 0;
        uint32_t ts_field = 0;
        bool extended = st.extended;  // fmt3 inherits
        if (fmt <= 2) {
            ts_field = be24(buffer_.data() + pos);
            extended = ts_field == 0xFFFFFF;
        }

        size_t ext_pos = pos + mh_len;
        if (extended && buffer_.size() < ext_pos + 4) return;
        uint32_t ext_value = 0;
        size_t header_total = ext_pos;
        if (extended) {
            ext_value = be32(buffer_.data() + ext_pos);
            header_total += 4;
        }

        // Apply the header to the chunk-stream state.
        if (fmt == 0) {
            st.timestamp = extended ? ext_value : ts_field;
            st.delta = 0;
            st.length = be24(buffer_.data() + pos + 3);
            st.type = buffer_[pos + 6];
            st.message_stream_id = le32(buffer_.data() + pos + 7);
        } else if (fmt == 1) {
            st.delta = extended ? ext_value : ts_field;
            st.timestamp += st.delta;
            st.length = be24(buffer_.data() + pos + 3);
            st.type = buffer_[pos + 6];
        } else if (fmt == 2) {
            st.delta = extended ? ext_value : ts_field;
            st.timestamp += st.delta;
        } else if (starting_new) {
            // fmt3 starting a new message re-applies the stored delta.
            st.timestamp += st.delta;
        }
        st.extended = extended;
        st.have_header = true;

        if (starting_new) {
            if (st.length > kMaxMessageSize) {
                failed_ = true;
                return;
            }
            st.remaining = st.length;
            st.assembling.clear();
            st.assembling.reserve(st.length);
        }

        const uint32_t take =
            st.remaining < chunk_size_ ? st.remaining : chunk_size_;
        if (buffer_.size() < header_total + take) return;

        st.assembling.insert(st.assembling.end(),
                             buffer_.begin() + static_cast<ptrdiff_t>(header_total),
                             buffer_.begin() + static_cast<ptrdiff_t>(header_total + take));
        st.remaining -= take;
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<ptrdiff_t>(header_total + take));

        if (st.remaining == 0) finalize(csid, st);
    }
}

}  // namespace roqr::rtmp
```

- [ ] **Step 4: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 78 prior + 7 new = 85.

- [ ] **Step 5: Commit**

```bash
git add rtmp tests
git commit -m "Add RTMP chunk reader with extended timestamps and control handling"
```

---
### Task 5: Chunk writer and round-trip

**Files:**
- Create: `rtmp/include/roqr/rtmp/chunk_writer.hpp`
- Create: `rtmp/src/chunk_writer.cpp`
- Modify: `rtmp/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/rtmp/chunk_writer_test.cpp`

**Interfaces:**
- Consumes: `RtmpMessage`, `kDefaultChunkSize`, `kTypeSetChunkSize` (Task 4); `ChunkReader` (tests only).
- Produces:

```cpp
namespace roqr::rtmp {
class ChunkWriter {
public:
    explicit ChunkWriter(uint32_t chunk_size = kDefaultChunkSize);
    // Appends the chunked encoding of msg (fmt0 first chunk + fmt3
    // continuations). Returns false for csid < 2 or > 65599.
    bool write(const RtmpMessage& msg, std::vector<uint8_t>& out);
    // Emits a Set Chunk Size protocol message (csid 2, msid 0), then
    // switches this writer to the new size.
    void set_chunk_size(uint32_t size, std::vector<uint8_t>& out);
    uint32_t chunk_size() const { return chunk_size_; }
private:
    uint32_t chunk_size_;
};
}
```

  Extended timestamps: when `msg.timestamp >= 0xFFFFFF` the fmt0 header carries 0xFFFFFF + a 4-byte value, and every fmt3 continuation repeats the 4-byte value (spec behavior; matches ffmpeg and Task 4's reader).

- [ ] **Step 1: Write the failing test `tests/rtmp/chunk_writer_test.cpp`**

```cpp
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
```

Add the test file and `src/chunk_writer.cpp` to the build.

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to compile (missing header).

- [ ] **Step 3: Implement**

`rtmp/include/roqr/rtmp/chunk_writer.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "roqr/rtmp/message.hpp"

namespace roqr::rtmp {

// Chunks RtmpMessages for the wire: fmt0 header on the first chunk, fmt3
// continuations after, extended timestamps repeated on continuations.
class ChunkWriter {
public:
    explicit ChunkWriter(uint32_t chunk_size = kDefaultChunkSize)
        : chunk_size_(chunk_size) {}

    bool write(const RtmpMessage& msg, std::vector<uint8_t>& out);
    void set_chunk_size(uint32_t size, std::vector<uint8_t>& out);
    uint32_t chunk_size() const { return chunk_size_; }

private:
    uint32_t chunk_size_;
};

}  // namespace roqr::rtmp
```

`rtmp/src/chunk_writer.cpp`:

```cpp
#include "roqr/rtmp/chunk_writer.hpp"

namespace roqr::rtmp {

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

void put_basic_header(uint8_t fmt, uint32_t csid, std::vector<uint8_t>& out) {
    if (csid < 64) {
        out.push_back(static_cast<uint8_t>(fmt << 6 | csid));
    } else if (csid < 320) {
        out.push_back(static_cast<uint8_t>(fmt << 6));
        out.push_back(static_cast<uint8_t>(csid - 64));
    } else {
        out.push_back(static_cast<uint8_t>(fmt << 6 | 1));
        out.push_back(static_cast<uint8_t>((csid - 64) & 0xFF));
        out.push_back(static_cast<uint8_t>((csid - 64) >> 8));
    }
}

}  // namespace

bool ChunkWriter::write(const RtmpMessage& msg, std::vector<uint8_t>& out) {
    if (msg.chunk_stream_id < 2 || msg.chunk_stream_id > 65599) return false;

    const bool extended = msg.timestamp >= 0xFFFFFF;

    put_basic_header(0, msg.chunk_stream_id, out);
    put_u24(extended ? 0xFFFFFF : msg.timestamp, out);
    put_u24(static_cast<uint32_t>(msg.payload.size()), out);
    out.push_back(msg.type);
    put_u32le(msg.message_stream_id, out);
    if (extended) put_u32be(msg.timestamp, out);

    size_t offset = 0;
    for (;;) {
        const size_t take =
            std::min<size_t>(chunk_size_, msg.payload.size() - offset);
        out.insert(out.end(), msg.payload.begin() + static_cast<ptrdiff_t>(offset),
                   msg.payload.begin() + static_cast<ptrdiff_t>(offset + take));
        offset += take;
        if (offset >= msg.payload.size()) break;
        put_basic_header(3, msg.chunk_stream_id, out);
        if (extended) put_u32be(msg.timestamp, out);
    }
    return true;
}

void ChunkWriter::set_chunk_size(uint32_t size, std::vector<uint8_t>& out) {
    RtmpMessage m;
    m.chunk_stream_id = 2;
    m.type = kTypeSetChunkSize;
    m.message_stream_id = 0;
    put_u32be(size, m.payload);
    write(m, out);
    chunk_size_ = size;
}

}  // namespace roqr::rtmp
```

(Add `#include <algorithm>` for `std::min`.)

- [ ] **Step 4: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 85 prior + 5 new = 90.

- [ ] **Step 5: Commit**

```bash
git add rtmp tests
git commit -m "Add RTMP chunk writer with round-trip tests"
```

---

### Task 6: Media classifier — legacy FLV headers

**Files:**
- Create: `rtmp/include/roqr/rtmp/classify.hpp`
- Create: `rtmp/src/classify.cpp`
- Modify: `rtmp/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/rtmp/classify_legacy_test.cpp`

**Interfaces:**
- Consumes: message type constants from Task 4.
- Produces:

```cpp
namespace roqr::rtmp {
enum class MediaClass {
    SequenceHeader,  // decoder config: must travel reliably
    Metadata,        // onMetaData / VideoPacketType.Metadata
    Keyframe,        // random access point coded frame
    Coded,           // other coded media
    Control,         // protocol control / sequence end / info frames
    Unknown,
};

struct MediaInfo {
    MediaClass cls = MediaClass::Unknown;
    bool enhanced = false;      // E-RTMP ex-header present
    bool multitrack = false;    // E-RTMP multitrack message
    bool force_stream = false;  // conservative: carry reliably (draft s10)
    uint32_t fourcc = 0;        // E-RTMP FourCC as big-endian u32, else 0
    uint8_t codec = 0;          // legacy video codec id / audio sound format
};

MediaInfo classify_video(std::span<const uint8_t> payload);
MediaInfo classify_audio(std::span<const uint8_t> payload);
// Dispatch on RTMP message type: 8 audio, 9 video, 15/18 data->Metadata,
// 1-6 -> Control, 22 aggregate -> Coded; anything else Unknown+force_stream.
MediaInfo classify(uint8_t message_type, std::span<const uint8_t> payload);
}
```

  Task 6 scope: legacy branch only. When the E-RTMP ex-header bit is present (video `payload[0] & 0x80`, audio sound format 9), return `{Unknown, enhanced=true, force_stream=true}` — Task 7 replaces that with real parsing. Legacy rules: video `frameType = (b0 >> 4) & 0x07`, `codec = b0 & 0x0F`; codec 7 (AVC) reads `b1` AVCPacketType (0 = sequence header, 2 = end-of-sequence -> Control); frameType 1 = keyframe; frameType 5 = video info/command -> Control. Audio: `soundFormat = b0 >> 4`; format 10 (AAC) reads `b1` (0 = sequence header); everything else Coded. Empty payloads -> Unknown + force_stream.

- [ ] **Step 1: Write the failing test `tests/rtmp/classify_legacy_test.cpp`**

```cpp
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
```

Add the test file and `src/classify.cpp` to the build.

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to compile (missing header).

- [ ] **Step 3: Implement**

`rtmp/include/roqr/rtmp/classify.hpp` — exactly the interface block above, with `#pragma once` and includes `<cstdint>`, `<span>`.

`rtmp/src/classify.cpp`:

```cpp
#include "roqr/rtmp/classify.hpp"

#include "roqr/rtmp/message.hpp"

namespace roqr::rtmp {

namespace {

MediaInfo unknown_reliable() {
    MediaInfo info;
    info.cls = MediaClass::Unknown;
    info.force_stream = true;
    return info;
}

// Task 7 replaces these with real E-RTMP v1/v2 parsing.
MediaInfo classify_video_enhanced(std::span<const uint8_t> payload) {
    (void)payload;
    MediaInfo info = unknown_reliable();
    info.enhanced = true;
    return info;
}

MediaInfo classify_audio_enhanced(std::span<const uint8_t> payload) {
    (void)payload;
    MediaInfo info = unknown_reliable();
    info.enhanced = true;
    return info;
}

}  // namespace

MediaInfo classify_video(std::span<const uint8_t> payload) {
    if (payload.empty()) return unknown_reliable();
    const uint8_t b0 = payload[0];
    if ((b0 & 0x80) != 0) return classify_video_enhanced(payload);

    MediaInfo info;
    const uint8_t frame_type = (b0 >> 4) & 0x07;
    info.codec = b0 & 0x0F;

    if (frame_type == 5) {  // video info / command frame
        info.cls = MediaClass::Control;
        return info;
    }
    if (info.codec == 7) {  // AVC: b1 is AVCPacketType
        if (payload.size() < 2) return unknown_reliable();
        if (payload[1] == 0) {
            info.cls = MediaClass::SequenceHeader;
            return info;
        }
        if (payload[1] == 2) {
            info.cls = MediaClass::Control;  // end of sequence
            return info;
        }
    }
    info.cls = frame_type == 1 ? MediaClass::Keyframe : MediaClass::Coded;
    return info;
}

MediaInfo classify_audio(std::span<const uint8_t> payload) {
    if (payload.empty()) return unknown_reliable();
    const uint8_t sound_format = payload[0] >> 4;
    if (sound_format == 9) return classify_audio_enhanced(payload);

    MediaInfo info;
    info.codec = sound_format;
    if (sound_format == 10) {  // AAC: b1 is AACPacketType
        if (payload.size() < 2) return unknown_reliable();
        info.cls = payload[1] == 0 ? MediaClass::SequenceHeader
                                   : MediaClass::Coded;
        return info;
    }
    info.cls = MediaClass::Coded;
    return info;
}

MediaInfo classify(uint8_t message_type, std::span<const uint8_t> payload) {
    switch (message_type) {
        case kTypeAudio:
            return classify_audio(payload);
        case kTypeVideo:
            return classify_video(payload);
        case 15:  // AMF3 data
        case kTypeDataAmf0: {
            MediaInfo info;
            info.cls = MediaClass::Metadata;
            return info;
        }
        case kTypeSetChunkSize:
        case kTypeAbort:
        case kTypeAcknowledgement:
        case kTypeUserControl:
        case kTypeWindowAckSize:
        case kTypeSetPeerBandwidth: {
            MediaInfo info;
            info.cls = MediaClass::Control;
            return info;
        }
        case 22: {  // aggregate: opaque coded media (spec: unpacking out of scope)
            MediaInfo info;
            info.cls = MediaClass::Coded;
            return info;
        }
        default:
            return unknown_reliable();
    }
}

}  // namespace roqr::rtmp
```

- [ ] **Step 4: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 90 prior + 4 new = 94.

- [ ] **Step 5: Commit**

```bash
git add rtmp tests
git commit -m "Add legacy FLV media classifier"
```

---

### Task 7: Media classifier — E-RTMP v1/v2 ex-headers

**Files:**
- Modify: `rtmp/src/classify.cpp` (replace the two `_enhanced` placeholders)
- Modify: `tests/CMakeLists.txt`
- Test: `tests/rtmp/classify_ertmp_test.cpp`
- Reference (read first, authoritative): `docs/reference/enhanced-rtmp-v2.md`, `docs/reference/enhanced-rtmp-v1.md`

**Interfaces:**
- Consumes: `MediaInfo`/`MediaClass` and the legacy dispatch from Task 6.
- Produces: real `classify_video_enhanced` / `classify_audio_enhanced`. Behavior (verify every enum value against the vendored spec per the Global Constraints fidelity rule):
  - Video ex-header: `b0 & 0x80` set; `frameType = (b0 >> 4) & 0x07`; `packetType = b0 & 0x0F`. VideoPacketType: 0 SequenceStart, 1 CodedFrames, 2 SequenceEnd, 3 CodedFramesX, 4 Metadata, 5 MPEG2TSSequenceStart, 6 Multitrack, 7 ModEx.
  - ModEx skip loop (both audio and video): `size = next_byte + 1`; if `size == 256` then `size = next_u16be + 1`; skip `size` bytes of ModEx data; the next byte's high nibble is the ModEx type (ignored) and low nibble is the effective packetType; repeat while packetType == ModEx.
  - Multitrack (video 6, audio 5): set `multitrack = true`, `force_stream = true` (conservative whole-message reliable carriage per the spec's classification rule); do not parse per-track detail; if the next byte's high nibble (AvMultitrackType) is not ManyTracksManyCodecs (2), the following 4 bytes are the shared FourCC — extract it when available.
  - Non-multitrack: the 4 bytes after the effective packetType byte(s) are the FourCC (store BE u32).
  - Class mapping — video: SequenceStart/MPEG2TSSequenceStart -> SequenceHeader; CodedFrames/CodedFramesX -> Keyframe when frameType == 1 else Coded; Metadata -> Metadata; SequenceEnd -> Control; unknown packet types -> Unknown + force_stream. Audio (AudioPacketType: 0 SequenceStart, 1 CodedFrames, 2 SequenceEnd, 4 MultichannelConfig, 5 Multitrack, 7 ModEx): SequenceStart/MultichannelConfig -> SequenceHeader; CodedFrames -> Coded; SequenceEnd -> Control; unknown -> Unknown + force_stream.
  - Truncated ex-headers at any point -> Unknown + force_stream (never read out of bounds).

- [ ] **Step 1: Read the vendored E-RTMP specs**

Read `docs/reference/enhanced-rtmp-v2.md` sections on the video ex-header, VideoPacketType, AudioPacketType, ModEx, and multitrack. Confirm or correct every enum value and the ModEx size encoding stated above. Record any correction in your report.

- [ ] **Step 2: Write the failing test `tests/rtmp/classify_ertmp_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include "roqr/rtmp/classify.hpp"

using namespace roqr::rtmp;

namespace {
constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24 |
           static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16 |
           static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8 |
           static_cast<uint8_t>(d);
}
}  // namespace

TEST_CASE("enhanced video sequence start extracts fourcc") {
    // IsExHeader | frameType 1 | packetType 0 (SequenceStart) + 'hvc1'
    const uint8_t p[] = {0x90, 'h', 'v', 'c', '1', 0x01, 0x02};
    auto info = classify_video(p);
    CHECK(info.cls == MediaClass::SequenceHeader);
    CHECK(info.enhanced);
    CHECK(info.fourcc == fourcc('h', 'v', 'c', '1'));
    CHECK_FALSE(info.force_stream);
}

TEST_CASE("enhanced video coded frames keyframe vs inter") {
    // frameType 1 keyframe, packetType 1 CodedFrames, av01
    const uint8_t key[] = {0x91, 'a', 'v', '0', '1', 0xDE};
    auto k = classify_video(key);
    CHECK(k.cls == MediaClass::Keyframe);
    CHECK(k.fourcc == fourcc('a', 'v', '0', '1'));

    // frameType 2 inter, packetType 3 CodedFramesX, vp09
    const uint8_t inter[] = {0xA3, 'v', 'p', '0', '9', 0xDE};
    CHECK(classify_video(inter).cls == MediaClass::Coded);
}

TEST_CASE("enhanced video metadata and sequence end") {
    const uint8_t meta[] = {0x94, 'h', 'v', 'c', '1', 0x02};
    CHECK(classify_video(meta).cls == MediaClass::Metadata);

    const uint8_t end[] = {0x92, 'h', 'v', 'c', '1'};
    CHECK(classify_video(end).cls == MediaClass::Control);
}

TEST_CASE("modex blocks are skipped to reach the effective packet type") {
    // packetType 7 (ModEx): size byte 0x02 -> 3 bytes of ModEx data,
    // then high nibble = ModEx type, low nibble = packetType 1
    // (CodedFrames), then fourcc.
    const uint8_t p[] = {0x97, 0x02, 0x00, 0x00, 0x00,
                         0x01, 'h',  'v',  'c',  '1', 0xAB};
    auto info = classify_video(p);
    CHECK(info.cls == MediaClass::Keyframe);  // frameType 1
    CHECK(info.enhanced);
    CHECK(info.fourcc == fourcc('h', 'v', 'c', '1'));
}

TEST_CASE("multitrack is conservative and forces stream carriage") {
    // packetType 6 (Multitrack); next byte: AvMultitrackType 0 (OneTrack)
    // << 4 | inner packetType 1; then shared fourcc.
    const uint8_t p[] = {0x96, 0x01, 'h', 'v', 'c', '1', 0x00};
    auto info = classify_video(p);
    CHECK(info.multitrack);
    CHECK(info.force_stream);
    CHECK(info.enhanced);
    CHECK(info.fourcc == fourcc('h', 'v', 'c', '1'));
}

TEST_CASE("enhanced audio packet types") {
    // soundFormat 9 (ExHeader) << 4 | AudioPacketType 0 SequenceStart
    const uint8_t seq[] = {0x90, 'a', 'c', '-', '3', 0x01};
    auto s = classify_audio(seq);
    CHECK(s.cls == MediaClass::SequenceHeader);
    CHECK(s.enhanced);
    CHECK(s.fourcc == fourcc('a', 'c', '-', '3'));

    const uint8_t coded[] = {0x91, 'O', 'p', 'u', 's', 0x00};
    CHECK(classify_audio(coded).cls == MediaClass::Coded);

    const uint8_t multichannel[] = {0x94, 'f', 'l', 'a', 'c', 0x02};
    CHECK(classify_audio(multichannel).cls == MediaClass::SequenceHeader);

    // AudioPacketType 5 Multitrack.
    const uint8_t multitrack[] = {0x95, 0x01, 'a', 'c', '-', '3'};
    auto m = classify_audio(multitrack);
    CHECK(m.multitrack);
    CHECK(m.force_stream);
}

TEST_CASE("truncated ex-headers degrade to Unknown reliable") {
    const uint8_t only_header[] = {0x90};  // fourcc missing
    auto v = classify_video(only_header);
    CHECK(v.cls == MediaClass::Unknown);
    CHECK(v.force_stream);

    const uint8_t modex_trunc[] = {0x97, 0x10, 0x00};  // ModEx data cut off
    CHECK(classify_video(modex_trunc).cls == MediaClass::Unknown);
}
```

Add `rtmp/classify_ertmp_test.cpp` to `roqr-rtmp-tests`.

- [ ] **Step 3: Run to verify RED**

Run: `cmake --build --preset dev && ctest --preset dev 2>&1 | tail -5`
Expected: new cases FAIL (placeholders return Unknown for everything, so the fourcc/class assertions fail).

- [ ] **Step 4: Implement — replace the two placeholders in `rtmp/src/classify.cpp`**

```cpp
namespace {

// Skips ModEx blocks (packetType 7). On success updates pos and returns
// the effective packet type; returns nullopt on truncation.
std::optional<uint8_t> skip_modex(std::span<const uint8_t> p, size_t& pos,
                                  uint8_t packet_type) {
    while (packet_type == 7) {
        if (pos >= p.size()) return std::nullopt;
        size_t size = static_cast<size_t>(p[pos++]) + 1;
        if (size == 256) {
            if (pos + 2 > p.size()) return std::nullopt;
            size = (static_cast<size_t>(p[pos]) << 8 | p[pos + 1]) + 1;
            pos += 2;
        }
        if (pos + size + 1 > p.size()) return std::nullopt;
        pos += size;
        packet_type = p[pos++] & 0x0F;  // high nibble: ModEx type (ignored)
    }
    return packet_type;
}

std::optional<uint32_t> read_fourcc(std::span<const uint8_t> p, size_t& pos) {
    if (pos + 4 > p.size()) return std::nullopt;
    const uint32_t f = static_cast<uint32_t>(p[pos]) << 24 |
                       static_cast<uint32_t>(p[pos + 1]) << 16 |
                       static_cast<uint32_t>(p[pos + 2]) << 8 | p[pos + 3];
    pos += 4;
    return f;
}

MediaInfo classify_video_enhanced(std::span<const uint8_t> p) {
    MediaInfo info;
    info.enhanced = true;
    const uint8_t frame_type = (p[0] >> 4) & 0x07;
    size_t pos = 1;

    auto packet_type = skip_modex(p, pos, p[0] & 0x0F);
    if (!packet_type) return unknown_reliable_enhanced();

    if (*packet_type == 6) {  // Multitrack: conservative
        info.multitrack = true;
        info.force_stream = true;
        info.cls = MediaClass::Coded;
        if (pos < p.size()) {
            const uint8_t av_multitrack_type = p[pos] >> 4;
            ++pos;
            if (av_multitrack_type != 2) {  // not ManyTracksManyCodecs
                if (auto f = read_fourcc(p, pos)) info.fourcc = *f;
            }
        }
        return info;
    }

    auto f = read_fourcc(p, pos);
    if (!f) return unknown_reliable_enhanced();
    info.fourcc = *f;

    switch (*packet_type) {
        case 0:  // SequenceStart
        case 5:  // MPEG2TSSequenceStart
            info.cls = MediaClass::SequenceHeader;
            break;
        case 1:  // CodedFrames
        case 3:  // CodedFramesX
            info.cls = frame_type == 1 ? MediaClass::Keyframe
                                       : MediaClass::Coded;
            break;
        case 2:  // SequenceEnd
            info.cls = MediaClass::Control;
            break;
        case 4:  // Metadata
            info.cls = MediaClass::Metadata;
            break;
        default:
            return unknown_reliable_enhanced();
    }
    return info;
}

MediaInfo classify_audio_enhanced(std::span<const uint8_t> p) {
    MediaInfo info;
    info.enhanced = true;
    size_t pos = 1;

    auto packet_type = skip_modex(p, pos, p[0] & 0x0F);
    if (!packet_type) return unknown_reliable_enhanced();

    if (*packet_type == 5) {  // Multitrack: conservative
        info.multitrack = true;
        info.force_stream = true;
        info.cls = MediaClass::Coded;
        if (pos < p.size()) {
            const uint8_t av_multitrack_type = p[pos] >> 4;
            ++pos;
            if (av_multitrack_type != 2) {
                if (auto f = read_fourcc(p, pos)) info.fourcc = *f;
            }
        }
        return info;
    }

    auto f = read_fourcc(p, pos);
    if (!f) return unknown_reliable_enhanced();
    info.fourcc = *f;

    switch (*packet_type) {
        case 0:  // SequenceStart
        case 4:  // MultichannelConfig
            info.cls = MediaClass::SequenceHeader;
            break;
        case 1:  // CodedFrames
            info.cls = MediaClass::Coded;
            break;
        case 2:  // SequenceEnd
            info.cls = MediaClass::Control;
            break;
        default:
            return unknown_reliable_enhanced();
    }
    return info;
}

MediaInfo unknown_reliable_enhanced() {
    MediaInfo info;
    info.cls = MediaClass::Unknown;
    info.force_stream = true;
    info.enhanced = true;
    return info;
}

}  // namespace
```

(Order the helper definitions so `unknown_reliable_enhanced` is declared before use; add `#include <optional>` to classify.cpp. The two `_enhanced` placeholder functions from Task 6 are replaced by these.)

- [ ] **Step 5: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 94 prior + 6 new = 100.

- [ ] **Step 6: Commit**

```bash
git add rtmp tests
git commit -m "Add E-RTMP ex-header parsing to the media classifier"
```

---

### Task 8: TCP Listener and ServerSession with loopback publish test

**Files:**
- Create: `rtmp/include/roqr/rtmp/server_session.hpp`
- Create: `rtmp/src/server_session.cpp`
- Modify: `rtmp/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/rtmp/server_session_test.cpp`

**Interfaces:**
- Consumes: `HandshakeResponder`/`HandshakeInitiator` (Task 3), `ChunkReader`/`ChunkWriter` (Tasks 4-5), `Amf0Value`/`amf0_encode`/`amf0_decode_all` (Tasks 1-2), `RtmpMessage` + type constants (Task 4).
- Produces:

```cpp
namespace roqr::rtmp {
class ServerSession;

// All callbacks fire on the session's own thread and must not block.
struct SessionEvents {
    std::function<void(ServerSession&, const RtmpMessage&)> on_message;
    std::function<void(ServerSession&, const std::string& app)> on_connect;
    std::function<void(ServerSession&, const std::string& stream_name,
                       bool publishing)> on_stream;
    std::function<void(ServerSession&)> on_close;
};

class ServerSession {
public:
    ServerSession(int fd, SessionEvents events);
    ~ServerSession();  // closes the socket
    void run();        // blocking: handshake, dechunk, dispatch until close
    bool send(const RtmpMessage& msg);  // thread-safe (chunks + writes)
    void close();      // shutdown(fd); run() returns soon after
    const std::string& app() const;
    const std::string& stream_name() const;
    bool publishing() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Listener {
public:
    using EventsFactory = std::function<SessionEvents(ServerSession&)>;
    Listener();
    ~Listener();  // calls stop()
    bool start(uint16_t port, EventsFactory factory);
    void stop();  // stop accepting, close sessions, join threads
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
```

  Built-in command handling inside `run()` (all commands are AMF0, type 20): `connect` → send Window Ack Size (2500000), Set Peer Bandwidth (2500000, dynamic=2), Set Chunk Size 4096, then `_result(txn, {fmsVer:"FMS/3,0,1,123", capabilities:31}, {level:"status", code:"NetConnection.Connect.Success", description:"Connection succeeded.", objectEncoding:0})`, fire `on_connect(app)`. `createStream` → `_result(txn, null, 1)`. `publish` → record stream name, send `onStatus` (msid 1) `NetStream.Publish.Start`, fire `on_stream(name, true)`. `play` → send User Control Stream Begin (event 0, stream 1), `onStatus` `NetStream.Play.Reset` then `NetStream.Play.Start`, fire `on_stream(name, false)`. Other commands and all media/data messages → `on_message`. Simple acknowledgement: send Acknowledgement (type 3) every 2500000 received bytes. Handshake or chunk failure → close, `on_close`, `run()` returns.

- [ ] **Step 1: Write the failing test `tests/rtmp/server_session_test.cpp`**

The test acts as an RTMP client using our own initiator + chunk codec over loopback TCP:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "roqr/rtmp/amf0.hpp"
#include "roqr/rtmp/chunk_reader.hpp"
#include "roqr/rtmp/chunk_writer.hpp"
#include "roqr/rtmp/handshake.hpp"
#include "roqr/rtmp/server_session.hpp"

using namespace roqr::rtmp;
using namespace std::chrono_literals;

namespace {

// Minimal blocking RTMP client for driving the server under test.
struct TestClient {
    int fd = -1;
    HandshakeInitiator hs;
    ChunkReader reader;
    ChunkWriter writer;

    bool connect_tcp(uint16_t port) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return ::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                         sizeof(addr)) == 0;
    }

    bool send_all(const std::vector<uint8_t>& data) {
        size_t off = 0;
        while (off < data.size()) {
            const ssize_t n =
                ::send(fd, data.data() + off, data.size() - off, 0);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    bool handshake() {
        if (!send_all(hs.start())) return false;
        uint8_t buf[4096];
        std::vector<uint8_t> c2;
        while (!hs.done()) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            if (!hs.feed(std::span<const uint8_t>(buf, static_cast<size_t>(n)),
                         c2)) {
                return false;
            }
            if (!c2.empty()) {
                if (!send_all(c2)) return false;
                c2.clear();
            }
        }
        return true;
    }

    bool send_command(std::vector<Amf0Value> values, uint32_t msid = 0) {
        RtmpMessage m;
        m.chunk_stream_id = 3;
        m.type = kTypeCommandAmf0;
        m.message_stream_id = msid;
        for (const auto& v : values) amf0_encode(v, m.payload);
        std::vector<uint8_t> wire;
        if (!writer.write(m, wire)) return false;
        return send_all(wire);
    }

    // Reads until a command with the given name arrives (applies Set
    // Chunk Size etc. via the reader). Bounded by attempts.
    std::optional<std::vector<Amf0Value>> await_command(
        const std::string& name) {
        uint8_t buf[4096];
        for (int i = 0; i < 200; ++i) {
            while (auto m = reader.next()) {
                if (m->type != kTypeCommandAmf0) continue;
                auto values = amf0_decode_all(m->payload);
                if (values && !values->empty() &&
                    (*values)[0].type() == Amf0Value::Type::String &&
                    (*values)[0].as_string() == name) {
                    return values;
                }
            }
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return std::nullopt;
            reader.feed(std::span<const uint8_t>(buf, static_cast<size_t>(n)));
        }
        return std::nullopt;
    }

    ~TestClient() {
        if (fd >= 0) ::close(fd);
    }
};

struct Events {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<RtmpMessage> messages;
    std::string app, stream;
    bool got_publish = false;

    SessionEvents make() {
        SessionEvents e;
        e.on_connect = [this](ServerSession&, const std::string& a) {
            std::lock_guard lock(mutex);
            app = a;
            cv.notify_all();
        };
        e.on_stream = [this](ServerSession&, const std::string& s, bool pub) {
            std::lock_guard lock(mutex);
            stream = s;
            got_publish = pub;
            cv.notify_all();
        };
        e.on_message = [this](ServerSession&, const RtmpMessage& m) {
            std::lock_guard lock(mutex);
            messages.push_back(m);
            cv.notify_all();
        };
        return e;
    }

    bool wait_messages(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout,
                           [&] { return messages.size() >= n; });
    }
};

}  // namespace

TEST_CASE("full publish flow over loopback TCP") {
    Events events;
    Listener listener;
    REQUIRE(listener.start(45570,
                           [&](ServerSession&) { return events.make(); }));

    TestClient client;
    REQUIRE(client.connect_tcp(45570));
    REQUIRE(client.handshake());

    Amf0Value cmd_obj = Amf0Value::object();
    cmd_obj.set("app", Amf0Value::string("live"))
        .set("tcUrl", Amf0Value::string("rtmp://127.0.0.1/live"));
    REQUIRE(client.send_command(
        {Amf0Value::string("connect"), Amf0Value::number(1), cmd_obj}));
    auto result = client.await_command("_result");
    REQUIRE(result.has_value());

    REQUIRE(client.send_command({Amf0Value::string("createStream"),
                                 Amf0Value::number(2), Amf0Value::null()}));
    auto cs = client.await_command("_result");
    REQUIRE(cs.has_value());
    REQUIRE(cs->size() >= 4);
    CHECK((*cs)[3].as_number() == 1.0);

    REQUIRE(client.send_command(
        {Amf0Value::string("publish"), Amf0Value::number(3),
         Amf0Value::null(), Amf0Value::string("mystream"),
         Amf0Value::string("live")},
        1));
    auto status = client.await_command("onStatus");
    REQUIRE(status.has_value());

    // Send an AVC sequence header and two video frames.
    RtmpMessage video;
    video.chunk_stream_id = 4;
    video.type = kTypeVideo;
    video.message_stream_id = 1;
    video.timestamp = 0;
    video.payload = {0x17, 0x00, 0x00, 0x00, 0x00};
    std::vector<uint8_t> wire;
    REQUIRE(client.writer.write(video, wire));
    video.timestamp = 40;
    video.payload = {0x17, 0x01, 0xAA};
    REQUIRE(client.writer.write(video, wire));
    video.timestamp = 80;
    video.payload = {0x27, 0x01, 0xBB};
    REQUIRE(client.writer.write(video, wire));
    REQUIRE(client.send_all(wire));

    REQUIRE(events.wait_messages(3, 5s));
    {
        std::lock_guard lock(events.mutex);
        CHECK(events.app == "live");
        CHECK(events.stream == "mystream");
        CHECK(events.got_publish);
        CHECK(events.messages[0].payload[1] == 0x00);
        CHECK(events.messages[1].timestamp == 40);
        CHECK(events.messages[2].timestamp == 80);
    }
    listener.stop();
}

TEST_CASE("play flow sends stream begin and play status") {
    Events events;
    Listener listener;
    REQUIRE(listener.start(45571,
                           [&](ServerSession&) { return events.make(); }));

    TestClient client;
    REQUIRE(client.connect_tcp(45571));
    REQUIRE(client.handshake());
    REQUIRE(client.send_command({Amf0Value::string("connect"),
                                 Amf0Value::number(1),
                                 Amf0Value::object()}));
    REQUIRE(client.await_command("_result").has_value());
    REQUIRE(client.send_command({Amf0Value::string("createStream"),
                                 Amf0Value::number(2), Amf0Value::null()}));
    REQUIRE(client.await_command("_result").has_value());
    REQUIRE(client.send_command(
        {Amf0Value::string("play"), Amf0Value::number(0), Amf0Value::null(),
         Amf0Value::string("mystream")},
        1));
    auto status = client.await_command("onStatus");
    REQUIRE(status.has_value());
    REQUIRE(status->size() >= 4);
    const Amf0Value* code = (*status)[3].find("code");
    REQUIRE(code != nullptr);
    // Reset arrives first; Start follows.
    CHECK((code->as_string() == "NetStream.Play.Reset" ||
           code->as_string() == "NetStream.Play.Start"));
    listener.stop();
}

TEST_CASE("garbage handshake closes the session without events") {
    Events events;
    Listener listener;
    REQUIRE(listener.start(45572,
                           [&](ServerSession&) { return events.make(); }));

    TestClient client;
    REQUIRE(client.connect_tcp(45572));
    std::vector<uint8_t> garbage(64, 0x55);
    REQUIRE(client.send_all(garbage));
    // Server must drop us; recv sees EOF within the bound.
    uint8_t buf[64];
    ssize_t n;
    do {
        n = ::recv(client.fd, buf, sizeof(buf), 0);
    } while (n > 0);
    CHECK(n == 0);
    {
        std::lock_guard lock(events.mutex);
        CHECK(events.app.empty());
        CHECK(events.messages.empty());
    }
    listener.stop();
}
```

Add the test file and `src/server_session.cpp` to the build.

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to compile (missing header).

- [ ] **Step 3: Implement `rtmp/include/roqr/rtmp/server_session.hpp`** — exactly the interface block above with `#pragma once` and includes (`<cstdint>`, `<functional>`, `<memory>`, `<string>`, `"roqr/rtmp/message.hpp"`).

- [ ] **Step 4: Implement `rtmp/src/server_session.cpp`**

```cpp
#include "roqr/rtmp/server_session.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "roqr/rtmp/amf0.hpp"
#include "roqr/rtmp/chunk_reader.hpp"
#include "roqr/rtmp/chunk_writer.hpp"
#include "roqr/rtmp/handshake.hpp"

namespace roqr::rtmp {

namespace {

constexpr uint32_t kAckWindow = 2'500'000;
constexpr uint32_t kOutChunkSize = 4096;

bool send_all(int fd, const std::vector<uint8_t>& data) {
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd, data.data() + off, data.size() - off,
                                 MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

RtmpMessage make_command(uint32_t csid, uint32_t msid,
                         const std::vector<Amf0Value>& values) {
    RtmpMessage m;
    m.chunk_stream_id = csid;
    m.type = kTypeCommandAmf0;
    m.message_stream_id = msid;
    for (const auto& v : values) amf0_encode(v, m.payload);
    return m;
}

Amf0Value status_info(const std::string& code, const std::string& desc) {
    Amf0Value info = Amf0Value::object();
    info.set("level", Amf0Value::string("status"))
        .set("code", Amf0Value::string(code))
        .set("description", Amf0Value::string(desc));
    return info;
}

RtmpMessage make_control(uint8_t type, std::vector<uint8_t> payload) {
    RtmpMessage m;
    m.chunk_stream_id = 2;
    m.type = type;
    m.message_stream_id = 0;
    m.payload = std::move(payload);
    return m;
}

std::vector<uint8_t> u32be(uint32_t v) {
    return {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
            static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
}

}  // namespace

struct ServerSession::Impl {
    int fd;
    SessionEvents events;
    HandshakeResponder handshake;
    ChunkReader reader;
    // writer + fd writes are guarded by write_mutex (send() is
    // thread-safe; command replies come from the session thread).
    std::mutex write_mutex;
    ChunkWriter writer;
    std::string app;
    std::string stream_name;
    bool publishing = false;
    uint64_t received = 0;
    uint64_t last_ack = 0;
    std::atomic<bool> closing{false};

    explicit Impl(int fd_in, SessionEvents ev)
        : fd(fd_in), events(std::move(ev)) {}

    bool send_message(const RtmpMessage& m) {
        std::lock_guard lock(write_mutex);
        std::vector<uint8_t> wire;
        if (!writer.write(m, wire)) return false;
        return send_all(fd, wire);
    }

    void send_chunk_size() {
        std::lock_guard lock(write_mutex);
        std::vector<uint8_t> wire;
        writer.set_chunk_size(kOutChunkSize, wire);
        send_all(fd, wire);
    }

    void handle_command(ServerSession& self, const RtmpMessage& m) {
        auto values = amf0_decode_all(m.payload);
        if (!values || values->empty() ||
            (*values)[0].type() != Amf0Value::Type::String) {
            return;  // unparseable command: ignore
        }
        const std::string& name = (*values)[0].as_string();
        const double txn =
            values->size() > 1 &&
                    (*values)[1].type() == Amf0Value::Type::Number
                ? (*values)[1].as_number()
                : 0;

        if (name == "connect") {
            if (values->size() > 2) {
                if (const Amf0Value* a = (*values)[2].find("app")) {
                    if (a->type() == Amf0Value::Type::String) {
                        app = a->as_string();
                    }
                }
            }
            send_message(make_control(kTypeWindowAckSize, u32be(kAckWindow)));
            auto bw = u32be(kAckWindow);
            bw.push_back(2);  // dynamic limit
            send_message(make_control(kTypeSetPeerBandwidth, std::move(bw)));
            send_chunk_size();

            Amf0Value props = Amf0Value::object();
            props.set("fmsVer", Amf0Value::string("FMS/3,0,1,123"))
                .set("capabilities", Amf0Value::number(31));
            Amf0Value info = status_info("NetConnection.Connect.Success",
                                         "Connection succeeded.");
            info.set("objectEncoding", Amf0Value::number(0));
            send_message(make_command(
                3, 0,
                {Amf0Value::string("_result"), Amf0Value::number(txn), props,
                 info}));
            if (events.on_connect) events.on_connect(self, app);
        } else if (name == "createStream") {
            send_message(make_command(
                3, 0,
                {Amf0Value::string("_result"), Amf0Value::number(txn),
                 Amf0Value::null(), Amf0Value::number(1)}));
        } else if (name == "publish") {
            if (values->size() > 3 &&
                (*values)[3].type() == Amf0Value::Type::String) {
                stream_name = (*values)[3].as_string();
            }
            publishing = true;
            send_message(make_command(
                5, 1,
                {Amf0Value::string("onStatus"), Amf0Value::number(0),
                 Amf0Value::null(),
                 status_info("NetStream.Publish.Start",
                             "Publishing " + stream_name)}));
            if (events.on_stream) events.on_stream(self, stream_name, true);
        } else if (name == "play") {
            if (values->size() > 3 &&
                (*values)[3].type() == Amf0Value::Type::String) {
                stream_name = (*values)[3].as_string();
            }
            // User Control Stream Begin (event 0) for stream 1.
            std::vector<uint8_t> begin = {0x00, 0x00};
            const auto sid = u32be(1);
            begin.insert(begin.end(), sid.begin(), sid.end());
            send_message(make_control(kTypeUserControl, std::move(begin)));
            send_message(make_command(
                5, 1,
                {Amf0Value::string("onStatus"), Amf0Value::number(0),
                 Amf0Value::null(),
                 status_info("NetStream.Play.Reset",
                             "Resetting " + stream_name)}));
            send_message(make_command(
                5, 1,
                {Amf0Value::string("onStatus"), Amf0Value::number(0),
                 Amf0Value::null(),
                 status_info("NetStream.Play.Start",
                             "Playing " + stream_name)}));
            if (events.on_stream) events.on_stream(self, stream_name, false);
        } else {
            if (events.on_message) events.on_message(self, m);
        }
    }

    void maybe_ack() {
        if (received - last_ack >= kAckWindow) {
            last_ack = received;
            send_message(make_control(
                kTypeAcknowledgement,
                u32be(static_cast<uint32_t>(received & 0xFFFFFFFF))));
        }
    }
};

ServerSession::ServerSession(int fd, SessionEvents events)
    : impl_(std::make_unique<Impl>(fd, std::move(events))) {}

ServerSession::~ServerSession() {
    if (impl_->fd >= 0) ::close(impl_->fd);
}

void ServerSession::run() {
    uint8_t buf[8192];
    // Handshake phase.
    while (!impl_->handshake.done()) {
        const ssize_t n = ::recv(impl_->fd, buf, sizeof(buf), 0);
        if (n <= 0 || impl_->closing) {
            if (impl_->events.on_close) impl_->events.on_close(*this);
            return;
        }
        std::vector<uint8_t> out;
        if (!impl_->handshake.feed(
                std::span<const uint8_t>(buf, static_cast<size_t>(n)), out)) {
            ::shutdown(impl_->fd, SHUT_RDWR);
            if (impl_->events.on_close) impl_->events.on_close(*this);
            return;
        }
        if (!out.empty() && !send_all(impl_->fd, out)) {
            if (impl_->events.on_close) impl_->events.on_close(*this);
            return;
        }
    }
    // Message phase.
    for (;;) {
        const ssize_t n = ::recv(impl_->fd, buf, sizeof(buf), 0);
        if (n <= 0 || impl_->closing) break;
        impl_->received += static_cast<uint64_t>(n);
        impl_->reader.feed(
            std::span<const uint8_t>(buf, static_cast<size_t>(n)));
        if (impl_->reader.failed()) break;
        while (auto m = impl_->reader.next()) {
            if (m->type == kTypeCommandAmf0) {
                impl_->handle_command(*this, *m);
            } else if (m->type == kTypeSetChunkSize ||
                       m->type == kTypeAbort ||
                       m->type == kTypeAcknowledgement ||
                       m->type == kTypeWindowAckSize ||
                       m->type == kTypeSetPeerBandwidth) {
                // Protocol control: reader already applied what matters.
            } else {
                if (impl_->events.on_message) {
                    impl_->events.on_message(*this, *m);
                }
            }
        }
        impl_->maybe_ack();
    }
    ::shutdown(impl_->fd, SHUT_RDWR);
    if (impl_->events.on_close) impl_->events.on_close(*this);
}

bool ServerSession::send(const RtmpMessage& msg) {
    return impl_->send_message(msg);
}

void ServerSession::close() {
    impl_->closing = true;
    ::shutdown(impl_->fd, SHUT_RDWR);
}

const std::string& ServerSession::app() const { return impl_->app; }
const std::string& ServerSession::stream_name() const {
    return impl_->stream_name;
}
bool ServerSession::publishing() const { return impl_->publishing; }

struct Listener::Impl {
    int listen_fd = -1;
    std::thread accept_thread;
    std::mutex mutex;
    std::vector<std::unique_ptr<ServerSession>> sessions;
    std::vector<std::thread> session_threads;
    EventsFactory factory;
    std::atomic<bool> running{false};
};

Listener::Listener() : impl_(std::make_unique<Impl>()) {}
Listener::~Listener() { stop(); }

bool Listener::start(uint16_t port, EventsFactory factory) {
    if (impl_->running) return false;
    impl_->factory = std::move(factory);
    impl_->listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (impl_->listen_fd < 0) return false;
    const int one = 1;
    ::setsockopt(impl_->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one,
                 sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(impl_->listen_fd, reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) != 0 ||
        ::listen(impl_->listen_fd, 8) != 0) {
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
        return false;
    }
    impl_->running = true;
    impl_->accept_thread = std::thread([this] {
        while (impl_->running) {
            const int fd = ::accept(impl_->listen_fd, nullptr, nullptr);
            if (fd < 0) break;  // listen socket closed by stop()
            std::lock_guard lock(impl_->mutex);
            auto session = std::make_unique<ServerSession>(
                fd, SessionEvents{});
            ServerSession* raw = session.get();
            // The factory sees the session before its thread starts.
            *session = ServerSession(fd, impl_->factory(*raw));
            impl_->sessions.push_back(std::move(session));
            impl_->session_threads.emplace_back(
                [raw] { raw->run(); });
        }
    });
    return true;
}

void Listener::stop() {
    if (!impl_->running) return;
    impl_->running = false;
    if (impl_->listen_fd >= 0) {
        ::shutdown(impl_->listen_fd, SHUT_RDWR);
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
    }
    if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
    {
        std::lock_guard lock(impl_->mutex);
        for (auto& s : impl_->sessions) s->close();
    }
    for (auto& t : impl_->session_threads) {
        if (t.joinable()) t.join();
    }
    std::lock_guard lock(impl_->mutex);
    impl_->sessions.clear();
    impl_->session_threads.clear();
}
```

**Implementation note (fix during Step 4, it will not compile as sketched):** the accept-loop line `*session = ServerSession(fd, impl_->factory(*raw));` is wrong — `ServerSession` is move-only via `unique_ptr` and reassigning would double-close the fd. Restructure: give `ServerSession` a private `set_events(SessionEvents)` method (declare `Listener::Impl` a friend, or simply make the events settable before `run()` starts):

```cpp
// server_session.hpp (public section)
    // Must be called before run(); Listener uses this so the events
    // factory can capture a reference to the constructed session.
    void set_events(SessionEvents events);
// server_session.cpp
void ServerSession::set_events(SessionEvents events) {
    impl_->events = std::move(events);
}
```

and in the accept loop:

```cpp
            auto session = std::make_unique<ServerSession>(fd, SessionEvents{});
            ServerSession* raw = session.get();
            raw->set_events(impl_->factory(*raw));
            impl_->sessions.push_back(std::move(session));
            impl_->session_threads.emplace_back([raw] { raw->run(); });
```

- [ ] **Step 5: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 100 prior + 3 new = 103. Also `ctest --preset dev --repeat until-fail:2` stable.

- [ ] **Step 6: Commit**

```bash
git add rtmp tests
git commit -m "Add RTMP TCP listener and server session with loopback publish test"
```

---

## Completion Criteria

- `ctest --preset dev` green: 103 test cases (63 baseline + 40 new), `--repeat until-fail:2` stable, warning-clean.
- `roqr-rtmp` builds with `ROQR_BUILD_QUIC=OFF` (no picoquic dependency).
- Spec coverage delivered: AMF0 codec (spec's listed types + long string), simple handshake both roles, chunk fmt 0-3 + extended timestamps + Set Chunk Size/Abort, E-RTMP v1/v2 classifier (ex-headers, ModEx skip, multitrack-conservative, FourCC), `ServerSession` with connect/createStream/publish/play handling and acknowledgements.
- Manual smoke (optional, not CI): `ffmpeg -re -f lavfi -i testsrc -c:v libx264 -f flv rtmp://127.0.0.1:45570/live/test` against a toy Listener main — Plan 4 automates this.

## Follow-On Plans

1. Plan 4: gateway examples (roqr-ingest/roqr-egress/roqr-duplex) wiring this module to the Plan 2 Client, relayd AMF0 command handling, ffmpeg e2e incl. E-RTMP HEVC case, GitHub Actions CI (+ TSAN job deferred from Plan 2).
2. Plan 5: C FFI (roqr.h, roqr_rtmp.h), JNI bindings, Java samples.
