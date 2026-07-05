# libroqr Plan 4: Gateways, Relay Command Handling, ffmpeg E2E, CI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The reference media path — ffmpeg publishes RTMP into `roqr-ingest`, frames cross QUIC to `roqr-relayd`, `roqr-egress` plays them back out as RTMP to ffmpeg — plus a real relay that routes by stream name, example binaries, an ffmpeg end-to-end test (including an E-RTMP HEVC case), and GitHub Actions CI.

**Architecture:** A `roqr-gateway` library holds the reusable, unit-testable pieces: `bridge` (RtmpMessage<->Frame with narrowing guards), `rtmp_commands` (build/parse connect/createStream/publish/play/_result/onStatus over AMF0), and the `IngestGateway`/`EgressGateway` classes that wire the Plan 3 RTMP `Listener`/`ServerSession` to the Plan 2 QUIC `Client`. `roqr-relayd` gains a sans-I/O `MediaRouter` and command handling so it behaves as a RoQR media server (publish/play by stream name). Integration tests drive real loopback QUIC + TCP using our own RTMP client codec; a shell script drives real ffmpeg processes end to end.

**Tech Stack:** C++20, roqr-core/quic/rtmp (existing), picoquic (via roqr-quic), Catch2 v3, CTest, ffmpeg (e2e only, gated), GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-07-04-libroqr-design.md` (data flow, gateway components). RoQR draft: `../roqr/draft-gregoire-rtmp-over-quic.txt` — s5 (flows), s7.3 (timestamp resolution), s8 (gap handling), s10 (mode choice).

## Global Constraints

- C++20; gateway namespace `roqr::gateway::`; include layout `gateway/include/roqr/gateway/*.hpp`, sources `gateway/src/*.cpp`; static lib `roqr-gateway`; tests target `roqr-gateway-tests` in `tests/gateway/`.
- CMake option `ROQR_BUILD_EXAMPLES` (ON by default) builds `roqr-gateway`, the example binaries, and the gateway tests; it requires `ROQR_BUILD_QUIC` and `ROQR_BUILD_RTMP` (both ON in the dev preset) and hard-errors if either is OFF.
- Warning flags on all new targets: `$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>`; builds stay warning-clean.
- **Width bridge rule (Plan 3 deferred finding):** `RtmpMessage` fields are `uint32_t`; `Frame` fields are `uint64_t`. RTMP->RoQR widening is lossless. RoQR->RTMP narrowing MUST be guarded: a `Frame` with `timestamp`, `message_stream_id`, or `chunk_stream_id` above `0xFFFFFFFF`, or `message_type` above `0xFF`, is rejected (conversion returns false), never silently truncated.
- **Delivery-mode policy at the ingest boundary (draft s10):** the ingest gateway classifies each media message (using `roqr::rtmp::classify`) and chooses `DeliveryMode`: RTMP commands, data/metadata, and sequence headers (`MediaClass::SequenceHeader`/`Metadata`/`Control` and `force_stream`) go `Stream`; audio/video coded frames go `Auto` (the Client resolves datagram-vs-stream). This is the spec's gateway classification duty.
- **Gap recovery at egress (draft s8, Plan 2 deferred `on_datagram_gap_hint`):** the egress gateway tracks per-flow RTMP timestamp continuity; on a suspected gap (a media timestamp regression or a large forward jump on a flow whose sequence header it has seen) it marks the flow discontinuous and drops non-keyframe video until the next `MediaClass::Keyframe` or `SequenceHeader`, then resumes. Detection is heuristic (RoQR frames carry no sequence numbers).
- Integration tests: loopback only, `127.0.0.1`, fixed ports 45580-45599 (one per test case), bounded waits (no unbounded loops; fail on timeout). Reuse the existing test cert fixture (`ROQR_TEST_CERT_DIR`).
- ffmpeg e2e test is gated: it self-skips (exit 0 with a SKIP message) when `ffmpeg`/`ffprobe` are not on `PATH`, so CI without ffmpeg still passes.
- Commit messages: plain imperative, no emoji, no Claude tagline, no Co-Authored-By. TDD per task.
- Build/test: `cmake --build --preset dev && ctest --preset dev` (if reconfigure is needed, run `eval "$(scripts/setup_picoquic_deps.sh)"` first). Baseline at plan start: 114 tests green.

---

### Task 1: Gateway module skeleton and RtmpMessage<->Frame bridge

**Files:**
- Create: `gateway/CMakeLists.txt`
- Create: `gateway/include/roqr/gateway/bridge.hpp`
- Create: `gateway/src/bridge.cpp`
- Modify: `CMakeLists.txt` (root: `ROQR_BUILD_EXAMPLES` option + subdir)
- Modify: `tests/CMakeLists.txt` (new `roqr-gateway-tests` target)
- Test: `tests/gateway/bridge_test.cpp`

**Interfaces:**
- Consumes: `roqr::Frame` (core), `roqr::rtmp::RtmpMessage` (rtmp).
- Produces:
  - `roqr::Frame roqr::gateway::to_frame(const roqr::rtmp::RtmpMessage&, uint64_t flow_id)` — widens all fields; empty payload allowed here is NOT (RoQR requires payload > 0), so an empty-payload message widens to a Frame with empty payload and the caller must not send it; document that.
  - `bool roqr::gateway::to_rtmp(const roqr::Frame&, roqr::rtmp::RtmpMessage& out)` — narrows with the Width bridge rule; returns false (out untouched) if any field exceeds its RTMP width.

- [ ] **Step 1: Create `gateway/include/roqr/gateway/bridge.hpp`**

```cpp
#pragma once

#include <cstdint>

#include "roqr/frame.hpp"
#include "roqr/rtmp/message.hpp"

namespace roqr::gateway {

// Widen an RTMP message into a RoQR frame on the given flow. All RTMP
// metadata fits in the wider RoQR fields, so this never fails. The RoQR
// timestamp carries the fully resolved RTMP message timestamp (draft
// s7.3); the caller must not send a frame with an empty payload (RoQR
// requires Payload Length > 0).
roqr::Frame to_frame(const roqr::rtmp::RtmpMessage& msg, uint64_t flow_id);

// Narrow a RoQR frame back into an RTMP message. Returns false and leaves
// out untouched if timestamp, message_stream_id, or chunk_stream_id exceeds
// 0xFFFFFFFF, or message_type exceeds 0xFF (the Width bridge rule).
bool to_rtmp(const roqr::Frame& frame, roqr::rtmp::RtmpMessage& out);

}  // namespace roqr::gateway
```

- [ ] **Step 2: Write the failing test `tests/gateway/bridge_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include "roqr/gateway/bridge.hpp"

using namespace roqr::gateway;

namespace {
roqr::rtmp::RtmpMessage rtmp_video(uint32_t ts) {
    roqr::rtmp::RtmpMessage m;
    m.timestamp = ts;
    m.type = 9;
    m.message_stream_id = 1;
    m.chunk_stream_id = 6;
    m.payload = {0x17, 0x01, 0xAA};
    return m;
}
}  // namespace

TEST_CASE("to_frame widens all metadata onto the flow") {
    const auto m = rtmp_video(1000);
    const roqr::Frame f = to_frame(m, 3);
    CHECK(f.flow_id == 3);
    CHECK(f.timestamp == 1000);
    CHECK(f.message_type == 9);
    CHECK(f.message_stream_id == 1);
    CHECK(f.chunk_stream_id == 6);
    CHECK(f.payload == m.payload);
}

TEST_CASE("to_rtmp narrows a well-formed frame") {
    roqr::Frame f = to_frame(rtmp_video(2000), 0);
    roqr::rtmp::RtmpMessage out;
    REQUIRE(to_rtmp(f, out));
    CHECK(out == rtmp_video(2000));
}

TEST_CASE("round-trip preserves every rtmp message") {
    const auto m = rtmp_video(0x00FF00FF);
    roqr::rtmp::RtmpMessage back;
    REQUIRE(to_rtmp(to_frame(m, 7), back));
    CHECK(back == m);
}

TEST_CASE("to_rtmp rejects fields that overflow RTMP widths") {
    roqr::Frame f = to_frame(rtmp_video(1), 0);
    roqr::rtmp::RtmpMessage out;

    f.timestamp = uint64_t{0xFFFFFFFF} + 1;
    CHECK_FALSE(to_rtmp(f, out));

    f = to_frame(rtmp_video(1), 0);
    f.message_stream_id = uint64_t{0xFFFFFFFF} + 1;
    CHECK_FALSE(to_rtmp(f, out));

    f = to_frame(rtmp_video(1), 0);
    f.chunk_stream_id = uint64_t{0xFFFFFFFF} + 1;
    CHECK_FALSE(to_rtmp(f, out));

    f = to_frame(rtmp_video(1), 0);
    f.message_type = 9;  // valid; message_type is already uint8 in Frame
    CHECK(to_rtmp(f, out));  // sanity: this one passes
}
```

(Note: `Frame::message_type` is already `uint8_t`, so it cannot overflow; the guard is only meaningful for the three varint fields. The last case documents that.)

- [ ] **Step 3: Wire the build**

`gateway/CMakeLists.txt`:

```cmake
add_library(roqr-gateway STATIC
  src/bridge.cpp
)

target_include_directories(roqr-gateway PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(roqr-gateway PUBLIC roqr-core roqr-quic roqr-rtmp)

target_compile_features(roqr-gateway PUBLIC cxx_std_20)
target_compile_options(roqr-gateway PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
```

Root `CMakeLists.txt`: add below the existing options:

```cmake
option(ROQR_BUILD_EXAMPLES "Build gateway library and example apps" ON)
```

and after the `add_subdirectory(rtmp)` block:

```cmake
if(ROQR_BUILD_EXAMPLES)
  if(NOT ROQR_BUILD_QUIC OR NOT ROQR_BUILD_RTMP)
    message(FATAL_ERROR "ROQR_BUILD_EXAMPLES requires ROQR_BUILD_QUIC and ROQR_BUILD_RTMP")
  endif()
  add_subdirectory(gateway)
  add_subdirectory(examples)
endif()
```

(Create `examples/CMakeLists.txt` as an empty placeholder now — Task 7 fills it — so the subdir add does not fail:)

`examples/CMakeLists.txt`:

```cmake
# Example binaries are added in Task 7.
```

`tests/CMakeLists.txt`: append after the rtmp block:

```cmake
if(ROQR_BUILD_EXAMPLES)
  add_executable(roqr-gateway-tests
    gateway/bridge_test.cpp
  )
  target_link_libraries(roqr-gateway-tests PRIVATE roqr-gateway Catch2::Catch2WithMain)
  catch_discover_tests(roqr-gateway-tests PROPERTIES TIMEOUT 60)
endif()
```

- [ ] **Step 4: Run to verify RED**

Run: `cmake --preset dev && cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to link (missing `to_frame`/`to_rtmp`).

- [ ] **Step 5: Implement `gateway/src/bridge.cpp`**

```cpp
#include "roqr/gateway/bridge.hpp"

namespace roqr::gateway {

roqr::Frame to_frame(const roqr::rtmp::RtmpMessage& msg, uint64_t flow_id) {
    roqr::Frame f;
    f.flow_id = flow_id;
    f.timestamp = msg.timestamp;
    f.message_type = msg.type;
    f.message_stream_id = msg.message_stream_id;
    f.chunk_stream_id = msg.chunk_stream_id;
    f.payload = msg.payload;
    return f;
}

bool to_rtmp(const roqr::Frame& frame, roqr::rtmp::RtmpMessage& out) {
    constexpr uint64_t kU32Max = 0xFFFFFFFF;
    if (frame.timestamp > kU32Max || frame.message_stream_id > kU32Max ||
        frame.chunk_stream_id > kU32Max) {
        return false;
    }
    out.timestamp = static_cast<uint32_t>(frame.timestamp);
    out.type = frame.message_type;
    out.message_stream_id = static_cast<uint32_t>(frame.message_stream_id);
    out.chunk_stream_id = static_cast<uint32_t>(frame.chunk_stream_id);
    out.payload = frame.payload;
    return true;
}

}  // namespace roqr::gateway
```

- [ ] **Step 6: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 114 prior + 4 new = 118.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt gateway examples tests
git commit -m "Add gateway module skeleton and RTMP-RoQR bridge"
```

---

### Task 2: RTMP command build/parse helpers

**Files:**
- Create: `gateway/include/roqr/gateway/rtmp_commands.hpp`
- Create: `gateway/src/rtmp_commands.cpp`
- Modify: `gateway/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/gateway/rtmp_commands_test.cpp`

**Interfaces:**
- Consumes: `roqr::rtmp::Amf0Value`, `amf0_encode`, `amf0_decode_all`, `RtmpMessage`, type constants (rtmp).
- Produces:
  - `struct roqr::gateway::Command { std::string name; double transaction_id = 0; std::vector<roqr::rtmp::Amf0Value> args; }` (args are values after the transaction id).
  - `std::optional<Command> parse_command(const roqr::rtmp::RtmpMessage&)` — returns nullopt if type != 20 (AMF0 command) or payload doesn't decode to name+txn.
  - Builders returning an `RtmpMessage` (chunk_stream_id 3, message_stream_id 0, type 20) except where noted:
    - `RtmpMessage build_connect(double txn, const std::string& app, const std::string& tc_url)`
    - `RtmpMessage build_create_stream(double txn)`
    - `RtmpMessage build_publish(double txn, const std::string& stream_name)` (message_stream_id 1)
    - `RtmpMessage build_play(double txn, const std::string& stream_name)` (message_stream_id 1)
    - `RtmpMessage build_result_object(double txn, roqr::rtmp::Amf0Value props, roqr::rtmp::Amf0Value info)` (the `connect` reply shape)
    - `RtmpMessage build_result_stream_id(double txn, double stream_id)` (the `createStream` reply)
    - `RtmpMessage build_on_status(const std::string& code, const std::string& description)` (message_stream_id 1)

- [ ] **Step 1: Write the failing test `tests/gateway/rtmp_commands_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include "roqr/gateway/rtmp_commands.hpp"

using namespace roqr::gateway;
using roqr::rtmp::Amf0Value;

TEST_CASE("build_connect round-trips through parse_command") {
    const auto msg = build_connect(1, "live", "rtmp://h/live");
    CHECK(msg.type == 20);
    CHECK(msg.chunk_stream_id == 3);

    auto cmd = parse_command(msg);
    REQUIRE(cmd.has_value());
    CHECK(cmd->name == "connect");
    CHECK(cmd->transaction_id == 1);
    REQUIRE(cmd->args.size() >= 1);
    REQUIRE(cmd->args[0].type() == Amf0Value::Type::Object);
    const Amf0Value* app = cmd->args[0].find("app");
    REQUIRE(app != nullptr);
    CHECK(app->as_string() == "live");
}

TEST_CASE("build_publish and build_play carry the stream name as arg") {
    auto pub = parse_command(build_publish(3, "cam"));
    REQUIRE(pub.has_value());
    CHECK(pub->name == "publish");
    // args: [null command object, "cam", ...]; find the first string arg.
    bool found = false;
    for (const auto& a : pub->args) {
        if (a.type() == Amf0Value::Type::String && a.as_string() == "cam") {
            found = true;
        }
    }
    CHECK(found);

    auto play = parse_command(build_play(4, "cam"));
    REQUIRE(play.has_value());
    CHECK(play->name == "play");
    CHECK(build_play(4, "cam").message_stream_id == 1);
}

TEST_CASE("build_create_stream and its result carry a stream id") {
    CHECK(parse_command(build_create_stream(2))->name == "createStream");
    auto res = parse_command(build_result_stream_id(2, 1));
    REQUIRE(res.has_value());
    CHECK(res->name == "_result");
    CHECK(res->transaction_id == 2);
    // The stream id is the value after null.
    bool has_one = false;
    for (const auto& a : res->args) {
        if (a.type() == Amf0Value::Type::Number && a.as_number() == 1.0) {
            has_one = true;
        }
    }
    CHECK(has_one);
}

TEST_CASE("build_on_status carries level status and the code") {
    auto st = parse_command(build_on_status("NetStream.Play.Start", "ok"));
    REQUIRE(st.has_value());
    CHECK(st->name == "onStatus");
    const Amf0Value* info = nullptr;
    for (const auto& a : st->args) {
        if (a.type() == Amf0Value::Type::Object && a.find("code") != nullptr) {
            info = &a;
        }
    }
    REQUIRE(info != nullptr);
    CHECK(info->find("code")->as_string() == "NetStream.Play.Start");
    CHECK(info->find("level")->as_string() == "status");
}

TEST_CASE("parse_command rejects non-command messages") {
    roqr::rtmp::RtmpMessage video;
    video.type = 9;
    video.payload = {0x17};
    CHECK_FALSE(parse_command(video).has_value());

    roqr::rtmp::RtmpMessage empty_cmd;
    empty_cmd.type = 20;  // no payload -> cannot decode name
    CHECK_FALSE(parse_command(empty_cmd).has_value());
}
```

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to compile (missing header).

- [ ] **Step 3: Implement**

`gateway/include/roqr/gateway/rtmp_commands.hpp`:

```cpp
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "roqr/rtmp/amf0.hpp"
#include "roqr/rtmp/message.hpp"

namespace roqr::gateway {

// A decoded RTMP AMF0 command: the command name, its transaction id, and
// every AMF0 value after the transaction id (command object, stream name,
// etc.).
struct Command {
    std::string name;
    double transaction_id = 0;
    std::vector<roqr::rtmp::Amf0Value> args;
};

std::optional<Command> parse_command(const roqr::rtmp::RtmpMessage& msg);

roqr::rtmp::RtmpMessage build_connect(double txn, const std::string& app,
                                      const std::string& tc_url);
roqr::rtmp::RtmpMessage build_create_stream(double txn);
roqr::rtmp::RtmpMessage build_publish(double txn,
                                      const std::string& stream_name);
roqr::rtmp::RtmpMessage build_play(double txn, const std::string& stream_name);
roqr::rtmp::RtmpMessage build_result_object(double txn,
                                            roqr::rtmp::Amf0Value props,
                                            roqr::rtmp::Amf0Value info);
roqr::rtmp::RtmpMessage build_result_stream_id(double txn, double stream_id);
roqr::rtmp::RtmpMessage build_on_status(const std::string& code,
                                        const std::string& description);

}  // namespace roqr::gateway
```

`gateway/src/rtmp_commands.cpp`:

```cpp
#include "roqr/gateway/rtmp_commands.hpp"

namespace roqr::gateway {

using roqr::rtmp::Amf0Value;
using roqr::rtmp::RtmpMessage;

namespace {

RtmpMessage command_message(const std::vector<Amf0Value>& values,
                            uint32_t message_stream_id) {
    RtmpMessage m;
    m.chunk_stream_id = 3;
    m.type = roqr::rtmp::kTypeCommandAmf0;
    m.message_stream_id = message_stream_id;
    for (const auto& v : values) roqr::rtmp::amf0_encode(v, m.payload);
    return m;
}

Amf0Value status_info(const std::string& code, const std::string& desc) {
    Amf0Value info = Amf0Value::object();
    info.set("level", Amf0Value::string("status"))
        .set("code", Amf0Value::string(code))
        .set("description", Amf0Value::string(desc));
    return info;
}

}  // namespace

std::optional<Command> parse_command(const RtmpMessage& msg) {
    if (msg.type != roqr::rtmp::kTypeCommandAmf0) return std::nullopt;
    auto values = roqr::rtmp::amf0_decode_all(msg.payload);
    if (!values || values->size() < 2) return std::nullopt;
    if ((*values)[0].type() != Amf0Value::Type::String) return std::nullopt;
    if ((*values)[1].type() != Amf0Value::Type::Number) return std::nullopt;

    Command cmd;
    cmd.name = (*values)[0].as_string();
    cmd.transaction_id = (*values)[1].as_number();
    cmd.args.assign(values->begin() + 2, values->end());
    return cmd;
}

RtmpMessage build_connect(double txn, const std::string& app,
                          const std::string& tc_url) {
    Amf0Value obj = Amf0Value::object();
    obj.set("app", Amf0Value::string(app))
        .set("tcUrl", Amf0Value::string(tc_url))
        .set("type", Amf0Value::string("nonprivate"));
    return command_message(
        {Amf0Value::string("connect"), Amf0Value::number(txn), obj}, 0);
}

RtmpMessage build_create_stream(double txn) {
    return command_message({Amf0Value::string("createStream"),
                            Amf0Value::number(txn), Amf0Value::null()},
                           0);
}

RtmpMessage build_publish(double txn, const std::string& stream_name) {
    return command_message(
        {Amf0Value::string("publish"), Amf0Value::number(txn),
         Amf0Value::null(), Amf0Value::string(stream_name),
         Amf0Value::string("live")},
        1);
}

RtmpMessage build_play(double txn, const std::string& stream_name) {
    return command_message(
        {Amf0Value::string("play"), Amf0Value::number(txn), Amf0Value::null(),
         Amf0Value::string(stream_name)},
        1);
}

RtmpMessage build_result_object(double txn, Amf0Value props, Amf0Value info) {
    return command_message({Amf0Value::string("_result"),
                            Amf0Value::number(txn), std::move(props),
                            std::move(info)},
                           0);
}

RtmpMessage build_result_stream_id(double txn, double stream_id) {
    return command_message({Amf0Value::string("_result"),
                            Amf0Value::number(txn), Amf0Value::null(),
                            Amf0Value::number(stream_id)},
                           0);
}

RtmpMessage build_on_status(const std::string& code,
                            const std::string& description) {
    return command_message(
        {Amf0Value::string("onStatus"), Amf0Value::number(0),
         Amf0Value::null(), status_info(code, description)},
        1);
}

}  // namespace roqr::gateway
```

Add `src/rtmp_commands.cpp` to `gateway/CMakeLists.txt` and `gateway/rtmp_commands_test.cpp` to `roqr-gateway-tests`.

- [ ] **Step 4: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 118 prior + 5 new = 123.

- [ ] **Step 5: Commit**

```bash
git add gateway tests
git commit -m "Add RTMP command build and parse helpers"
```

---

### Task 3: Relay media router (sans-I/O)

**Files:**
- Create: `tools/relayd/include/roqr/relayd/media_router.hpp`
- Create: `tools/relayd/src/media_router.cpp`
- Modify: `tools/relayd/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/gateway/media_router_test.cpp`

**Interfaces:**
- Consumes: nothing (opaque connection handles are `void*`).
- Produces:

```cpp
namespace roqr::relayd {
// Routes RoQR media by stream name. Connection identity is an opaque
// void* (the picoquic cnx pointer in the server; a fake handle in tests).
// Sans-I/O: it decides *who* should receive a frame, never sends.
class MediaRouter {
public:
    void register_publisher(void* conn, const std::string& stream_name);
    void register_subscriber(void* conn, const std::string& stream_name);
    // Subscribers of the stream this publisher owns (empty if conn is not a
    // known publisher). Excludes the publisher itself.
    std::vector<void*> subscribers_for_publisher(void* conn) const;
    // Cache the latest sequence-header/metadata frame for a publisher's
    // stream so a late subscriber can be primed. Keyed by (stream, kind).
    void cache_init(const std::string& stream_name, uint8_t message_type,
                    std::vector<uint8_t> frame_bytes);
    // Init frames (metadata + audio/video sequence headers) to replay to a
    // newly-registered subscriber, in insertion order.
    std::vector<std::vector<uint8_t>> init_frames(
        const std::string& stream_name) const;
    // Drop all state for a closed connection (publisher or subscriber).
    void remove(void* conn);
    std::string stream_of(void* conn) const;  // "" if unknown
};
}
```

- [ ] **Step 1: Write the failing test `tests/gateway/media_router_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include "roqr/relayd/media_router.hpp"

using namespace roqr::relayd;

namespace {
void* handle(uintptr_t v) { return reinterpret_cast<void*>(v); }
}  // namespace

TEST_CASE("subscribers are routed by stream name, publisher excluded") {
    MediaRouter r;
    r.register_publisher(handle(1), "cam");
    r.register_subscriber(handle(2), "cam");
    r.register_subscriber(handle(3), "cam");
    r.register_subscriber(handle(4), "other");

    auto subs = r.subscribers_for_publisher(handle(1));
    REQUIRE(subs.size() == 2);
    CHECK((subs[0] == handle(2) || subs[0] == handle(3)));
    CHECK(subs[0] != handle(1));

    CHECK(r.subscribers_for_publisher(handle(99)).empty());  // unknown
}

TEST_CASE("init frames replay in insertion order") {
    MediaRouter r;
    r.register_publisher(handle(1), "cam");
    r.cache_init("cam", 18, {0x01});  // metadata
    r.cache_init("cam", 9, {0x02});   // video seq header
    r.cache_init("cam", 8, {0x03});   // audio seq header
    // Re-caching the same message type replaces it.
    r.cache_init("cam", 9, {0x22});

    auto frames = r.init_frames("cam");
    REQUIRE(frames.size() == 3);
    CHECK(frames[0] == std::vector<uint8_t>{0x01});
    CHECK(frames[1] == std::vector<uint8_t>{0x22});
    CHECK(frames[2] == std::vector<uint8_t>{0x03});

    CHECK(r.init_frames("missing").empty());
}

TEST_CASE("remove drops publisher and subscriber state") {
    MediaRouter r;
    r.register_publisher(handle(1), "cam");
    r.register_subscriber(handle(2), "cam");
    CHECK(r.stream_of(handle(2)) == "cam");

    r.remove(handle(2));
    CHECK(r.subscribers_for_publisher(handle(1)).empty());
    CHECK(r.stream_of(handle(2)).empty());

    r.remove(handle(1));
    CHECK(r.stream_of(handle(1)).empty());
    r.register_subscriber(handle(3), "cam");
    CHECK(r.subscribers_for_publisher(handle(1)).empty());  // no publisher
}
```

Add `gateway/media_router_test.cpp` to `roqr-gateway-tests` and link `roqr-relayd-lib` into that test target (add it to the `target_link_libraries` for `roqr-gateway-tests`).

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to compile (missing header).

- [ ] **Step 3: Implement**

`tools/relayd/include/roqr/relayd/media_router.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace roqr::relayd {

class MediaRouter {
public:
    void register_publisher(void* conn, const std::string& stream_name);
    void register_subscriber(void* conn, const std::string& stream_name);
    std::vector<void*> subscribers_for_publisher(void* conn) const;
    void cache_init(const std::string& stream_name, uint8_t message_type,
                    std::vector<uint8_t> frame_bytes);
    std::vector<std::vector<uint8_t>> init_frames(
        const std::string& stream_name) const;
    void remove(void* conn);
    std::string stream_of(void* conn) const;

private:
    struct Stream {
        void* publisher = nullptr;
        std::vector<void*> subscribers;
        std::vector<uint8_t> init_types;              // insertion order
        std::map<uint8_t, std::vector<uint8_t>> init;  // by message type
    };
    std::map<std::string, Stream> streams_;
    std::map<void*, std::string> conn_stream_;  // conn -> stream name
};

}  // namespace roqr::relayd
```

`tools/relayd/src/media_router.cpp`:

```cpp
#include "roqr/relayd/media_router.hpp"

#include <algorithm>

namespace roqr::relayd {

void MediaRouter::register_publisher(void* conn,
                                     const std::string& stream_name) {
    streams_[stream_name].publisher = conn;
    conn_stream_[conn] = stream_name;
}

void MediaRouter::register_subscriber(void* conn,
                                      const std::string& stream_name) {
    auto& s = streams_[stream_name];
    if (std::find(s.subscribers.begin(), s.subscribers.end(), conn) ==
        s.subscribers.end()) {
        s.subscribers.push_back(conn);
    }
    conn_stream_[conn] = stream_name;
}

std::vector<void*> MediaRouter::subscribers_for_publisher(void* conn) const {
    auto it = conn_stream_.find(conn);
    if (it == conn_stream_.end()) return {};
    auto sit = streams_.find(it->second);
    if (sit == streams_.end() || sit->second.publisher != conn) return {};
    return sit->second.subscribers;
}

void MediaRouter::cache_init(const std::string& stream_name,
                             uint8_t message_type,
                             std::vector<uint8_t> frame_bytes) {
    auto& s = streams_[stream_name];
    if (s.init.find(message_type) == s.init.end()) {
        s.init_types.push_back(message_type);
    }
    s.init[message_type] = std::move(frame_bytes);
}

std::vector<std::vector<uint8_t>> MediaRouter::init_frames(
    const std::string& stream_name) const {
    auto it = streams_.find(stream_name);
    if (it == streams_.end()) return {};
    std::vector<std::vector<uint8_t>> out;
    for (uint8_t type : it->second.init_types) {
        out.push_back(it->second.init.at(type));
    }
    return out;
}

void MediaRouter::remove(void* conn) {
    auto it = conn_stream_.find(conn);
    if (it == conn_stream_.end()) return;
    auto sit = streams_.find(it->second);
    if (sit != streams_.end()) {
        Stream& s = sit->second;
        if (s.publisher == conn) s.publisher = nullptr;
        s.subscribers.erase(
            std::remove(s.subscribers.begin(), s.subscribers.end(), conn),
            s.subscribers.end());
    }
    conn_stream_.erase(it);
}

std::string MediaRouter::stream_of(void* conn) const {
    auto it = conn_stream_.find(conn);
    return it == conn_stream_.end() ? std::string() : it->second;
}

}  // namespace roqr::relayd
```

Add `src/media_router.cpp` to `roqr-relayd-lib` in `tools/relayd/CMakeLists.txt`.

- [ ] **Step 4: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 123 prior + 3 new = 126.

- [ ] **Step 5: Commit**

```bash
git add tools tests
git commit -m "Add relay media router keyed by stream name"
```

---
### Task 4: Relay command handling and media routing over QUIC

**Files:**
- Modify: `tools/relayd/include/roqr/relayd/server.hpp` (Mode gains a real Media mode; keep Echo/Relay)
- Modify: `tools/relayd/src/server.cpp` (wire MediaRouter + command handling into the connection callback)
- Modify: `tests/CMakeLists.txt` (add integration test)
- Test: `tests/integration/relay_media_test.cpp`

**Interfaces:**
- Consumes: `MediaRouter` (Task 3); `roqr::gateway::parse_command`, `build_result_object`, `build_result_stream_id`, `build_on_status` (Task 2); `to_frame`/`to_rtmp` (Task 1); `roqr::Frame`, `frame_encode`, `FrameDecoder`, `datagram_decode`; `roqr::rtmp::RtmpMessage`, `Amf0Value`, `kTypeCommandAmf0`.
- Produces: `Mode::Media` — the relay parses RoQR frames carrying RTMP commands (`connect`/`createStream`/`publish`/`play`) on any connection, replies with `_result`/`onStatus` as RoQR frames on the same stream, registers publishers/subscribers by stream name, replays cached init frames to new subscribers, and forwards media frames publisher->subscribers preserving carriage. All frames use flow 0 in this task (the reference deployment; draft s5 permits carrying the whole session on flow 0).

- [ ] **Step 1: Write the failing test `tests/integration/relay_media_test.cpp`**

The test drives two RoQR `Client`s (publisher, subscriber) against a `Mode::Media` relay, using the gateway command builders directly:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "roqr/gateway/bridge.hpp"
#include "roqr/gateway/rtmp_commands.hpp"
#include "roqr/quic/client.hpp"
#include "roqr/relayd/server.hpp"

using namespace std::chrono_literals;
using roqr::gateway::to_frame;

namespace {
const std::string kCertDir = ROQR_TEST_CERT_DIR;

struct Collector {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<roqr::Frame> frames;
    void add(const roqr::Frame& f) {
        std::lock_guard lock(mutex);
        frames.push_back(f);
        cv.notify_all();
    }
    bool wait_count(size_t n, std::chrono::milliseconds t) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, t, [&] { return frames.size() >= n; });
    }
    size_t count() {
        std::lock_guard lock(mutex);
        return frames.size();
    }
};

roqr::relayd::ServerOptions media_opts(uint16_t port) {
    roqr::relayd::ServerOptions o;
    o.port = port;
    o.cert_file = kCertDir + "/cert.pem";
    o.key_file = kCertDir + "/key.pem";
    o.mode = roqr::relayd::Mode::Media;
    return o;
}

void send_cmd(roqr::quic::Client& c, const roqr::rtmp::RtmpMessage& m) {
    c.send(to_frame(m, 0), roqr::quic::DeliveryMode::Stream);
}

roqr::rtmp::RtmpMessage media(uint8_t type, uint32_t ts,
                              std::vector<uint8_t> payload) {
    roqr::rtmp::RtmpMessage m;
    m.type = type;
    m.timestamp = ts;
    m.message_stream_id = 1;
    m.chunk_stream_id = type == 8 ? 4 : 6;
    m.payload = std::move(payload);
    return m;
}
}  // namespace

TEST_CASE("media relay forwards a published stream to a subscriber") {
    roqr::relayd::Server server;
    REQUIRE(server.start(media_opts(45580)));

    // Subscriber connects and plays first.
    Collector sub_got;
    roqr::quic::Client sub;
    sub.on_message([&](const roqr::Frame& f) { sub_got.add(f); });
    REQUIRE(sub.connect("127.0.0.1", 45580));
    REQUIRE(sub.wait_connected(5s));
    send_cmd(sub, roqr::gateway::build_connect(1, "live", "rtmp://h/live"));
    send_cmd(sub, roqr::gateway::build_create_stream(2));
    send_cmd(sub, roqr::gateway::build_play(3, "cam"));

    // Publisher connects, publishes, sends a video seq header + a frame.
    roqr::quic::Client pub;
    REQUIRE(pub.connect("127.0.0.1", 45580));
    REQUIRE(pub.wait_connected(5s));
    send_cmd(pub, roqr::gateway::build_connect(1, "live", "rtmp://h/live"));
    send_cmd(pub, roqr::gateway::build_create_stream(2));
    send_cmd(pub, roqr::gateway::build_publish(3, "cam"));
    // Give the publish command time to register before media flows.
    std::this_thread::sleep_for(200ms);
    pub.send(to_frame(media(9, 0, {0x17, 0x00, 0x01}), 0),
             roqr::quic::DeliveryMode::Stream);  // AVC seq header
    pub.send(to_frame(media(9, 40, {0x17, 0x01, 0xAA}), 0),
             roqr::quic::DeliveryMode::Stream);  // keyframe

    // Subscriber should receive the _result/onStatus replies plus the two
    // media frames. Assert at least the two video frames arrive.
    REQUIRE(sub_got.wait_count(2, 5s));
    bool saw_seq_header = false, saw_keyframe = false;
    {
        std::lock_guard lock(sub_got.mutex);
        for (const auto& f : sub_got.frames) {
            if (f.message_type == 9 && f.payload.size() >= 2) {
                if (f.payload[1] == 0x00) saw_seq_header = true;
                if (f.payload[1] == 0x01) saw_keyframe = true;
            }
        }
    }
    CHECK(saw_seq_header);
    CHECK(saw_keyframe);

    pub.close();
    sub.close();
    pub.wait_closed(5s);
    sub.wait_closed(5s);
    server.stop();
}

TEST_CASE("a late subscriber is primed with the cached sequence header") {
    roqr::relayd::Server server;
    REQUIRE(server.start(media_opts(45581)));

    roqr::quic::Client pub;
    REQUIRE(pub.connect("127.0.0.1", 45581));
    REQUIRE(pub.wait_connected(5s));
    send_cmd(pub, roqr::gateway::build_connect(1, "live", "rtmp://h/live"));
    send_cmd(pub, roqr::gateway::build_create_stream(2));
    send_cmd(pub, roqr::gateway::build_publish(3, "cam"));
    std::this_thread::sleep_for(200ms);
    pub.send(to_frame(media(9, 0, {0x17, 0x00, 0x99}), 0),
             roqr::quic::DeliveryMode::Stream);  // seq header, cached
    std::this_thread::sleep_for(200ms);

    // Subscriber joins AFTER the seq header was published.
    Collector sub_got;
    roqr::quic::Client sub;
    sub.on_message([&](const roqr::Frame& f) { sub_got.add(f); });
    REQUIRE(sub.connect("127.0.0.1", 45581));
    REQUIRE(sub.wait_connected(5s));
    send_cmd(sub, roqr::gateway::build_connect(1, "live", "rtmp://h/live"));
    send_cmd(sub, roqr::gateway::build_create_stream(2));
    send_cmd(sub, roqr::gateway::build_play(3, "cam"));

    // The cached seq header must be replayed on play, even though the
    // subscriber missed the live one.
    REQUIRE(sub_got.wait_count(1, 5s));
    bool primed = false;
    {
        std::lock_guard lock(sub_got.mutex);
        for (const auto& f : sub_got.frames) {
            if (f.message_type == 9 && f.payload.size() >= 3 &&
                f.payload[2] == 0x99) {
                primed = true;
            }
        }
    }
    CHECK(primed);

    pub.close();
    sub.close();
    pub.wait_closed(5s);
    sub.wait_closed(5s);
    server.stop();
}
```

Add `integration/relay_media_test.cpp` to `roqr-integration-tests`, and link `roqr-gateway` into that target.

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to compile (`Mode::Media` does not exist).

- [ ] **Step 3: Add `Mode::Media` to `tools/relayd/include/roqr/relayd/server.hpp`**

```cpp
// Media: parse RTMP commands carried as RoQR frames, register publishers
// and subscribers by stream name, replay cached init frames on play, and
// route media publisher->subscribers (the reference RoQR media server).
enum class Mode { Echo, Relay, Media };
```

- [ ] **Step 4: Wire command handling and routing into `tools/relayd/src/server.cpp`**

Add includes:

```cpp
#include "roqr/gateway/bridge.hpp"
#include "roqr/gateway/rtmp_commands.hpp"
#include "roqr/relayd/media_router.hpp"
#include "roqr/rtmp/amf0.hpp"
```

Add a `MediaRouter router;` member to `Server::Impl`. Add a per-connection helper that sends a `roqr::rtmp::RtmpMessage` to a specific cnx as a RoQR frame on flow 0's stream (mirror the existing `forward_frame` stream path):

```cpp
    void send_rtmp(picoquic_cnx_t* cnx, const roqr::rtmp::RtmpMessage& msg,
                   uint64_t stream_id) {
        std::vector<uint8_t> wire;
        if (!roqr::frame_encode(roqr::gateway::to_frame(msg, 0), wire)) return;
        picoquic_add_to_stream(cnx, stream_id, wire.data(), wire.size(), 0);
    }
```

Add a media-mode frame handler. When a decoded `roqr::Frame f` arrives on `cnx` over `stream_id` (stream) or as a datagram, dispatch:

```cpp
    void handle_media_frame(picoquic_cnx_t* cnx, uint64_t stream_id,
                            const roqr::Frame& f, bool as_datagram) {
        roqr::rtmp::RtmpMessage msg;
        if (!roqr::gateway::to_rtmp(f, msg)) return;  // malformed width

        if (msg.type == roqr::rtmp::kTypeCommandAmf0) {
            auto cmd = roqr::gateway::parse_command(msg);
            if (!cmd) return;
            if (cmd->name == "connect") {
                roqr::rtmp::Amf0Value props = roqr::rtmp::Amf0Value::object();
                props.set("fmsVer",
                          roqr::rtmp::Amf0Value::string("FMS/3,0,1,123"))
                    .set("capabilities", roqr::rtmp::Amf0Value::number(31));
                roqr::rtmp::Amf0Value info = roqr::rtmp::Amf0Value::object();
                info.set("level", roqr::rtmp::Amf0Value::string("status"))
                    .set("code", roqr::rtmp::Amf0Value::string(
                                     "NetConnection.Connect.Success"))
                    .set("description", roqr::rtmp::Amf0Value::string("ok"))
                    .set("objectEncoding", roqr::rtmp::Amf0Value::number(0));
                send_rtmp(cnx,
                          roqr::gateway::build_result_object(
                              cmd->transaction_id, props, info),
                          stream_id);
            } else if (cmd->name == "createStream") {
                send_rtmp(cnx,
                          roqr::gateway::build_result_stream_id(
                              cmd->transaction_id, 1),
                          stream_id);
            } else if (cmd->name == "publish") {
                std::string name = command_stream_name(*cmd);
                router.register_publisher(cnx, name);
                send_rtmp(cnx,
                          roqr::gateway::build_on_status(
                              "NetStream.Publish.Start", "publishing"),
                          stream_id);
            } else if (cmd->name == "play") {
                std::string name = command_stream_name(*cmd);
                router.register_subscriber(cnx, name);
                send_rtmp(cnx,
                          roqr::gateway::build_on_status(
                              "NetStream.Play.Start", "playing"),
                          stream_id);
                // Prime the new subscriber with cached init frames.
                for (auto& bytes : router.init_frames(name)) {
                    picoquic_add_to_stream(cnx, stream_id, bytes.data(),
                                           bytes.size(), 0);
                }
            }
            return;
        }

        // Media/data: cache init frames, then route to subscribers.
        if (is_init_frame(msg)) {
            std::vector<uint8_t> bytes;
            if (roqr::frame_encode(f, bytes)) {
                router.cache_init(router.stream_of(cnx), msg.type,
                                  std::move(bytes));
            }
        }
        std::vector<uint8_t> wire;
        if (!roqr::frame_encode(f, wire)) return;
        for (void* sub : router.subscribers_for_publisher(cnx)) {
            auto* scnx = static_cast<picoquic_cnx_t*>(sub);
            if (as_datagram) {
                picoquic_queue_datagram_frame(scnx, wire.size(), wire.data());
            } else {
                // Forward on the subscriber's flow-0 stream (id 0).
                picoquic_add_to_stream(scnx, 0, wire.data(), wire.size(), 0);
            }
        }
    }
```

with two file-local helpers (anonymous namespace):

```cpp
std::string command_stream_name(const roqr::gateway::Command& cmd) {
    for (const auto& a : cmd.args) {
        if (a.type() == roqr::rtmp::Amf0Value::Type::String) {
            return a.as_string();
        }
    }
    return {};
}

// Init frames: onMetaData (18/15) and AVC/AAC/E-RTMP sequence headers.
bool is_init_frame(const roqr::rtmp::RtmpMessage& msg) {
    if (msg.type == 18 || msg.type == 15) return true;
    if (msg.type == 8 || msg.type == 9) {
        const auto info = msg.type == 9
                              ? roqr::rtmp::classify_video(msg.payload)
                              : roqr::rtmp::classify_audio(msg.payload);
        return info.cls == roqr::rtmp::MediaClass::SequenceHeader;
    }
    return false;
}
```

(add `#include "roqr/rtmp/classify.hpp"`).

In the connection callback's stream and datagram cases, when `options.mode == Mode::Media`, route decoded frames through `handle_media_frame` instead of the Echo/Relay `forward_frame`. In the stream path you already run a per-(cnx,stream) `FrameDecoder`; for each `decoder.next()` frame call `handle_media_frame(cnx, stream_id, *frame, false)`. In the datagram path, after `datagram_decode` succeeds call `handle_media_frame(cnx, 0, frame, true)`. On connection close, additionally call `router.remove(cnx)`.

Link `roqr-gateway` into `roqr-relayd-lib` (`tools/relayd/CMakeLists.txt` `target_link_libraries(roqr-relayd-lib PUBLIC roqr-quic roqr-gateway)`).

- [ ] **Step 5: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 126 prior + 2 new = 128. If routing races (media sent before the subscriber's play registers), the 200ms settle in the test covers the publisher side; keep the subscriber's play before the publisher's media in the first test.

- [ ] **Step 6: Commit**

```bash
git add tools tests
git commit -m "Add media mode to relay with command handling and routing"
```

---

### Task 5: Ingest gateway (RTMP publish -> RoQR)

**Files:**
- Create: `gateway/include/roqr/gateway/ingest.hpp`
- Create: `gateway/src/ingest.cpp`
- Modify: `gateway/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/integration/ingest_test.cpp`

**Interfaces:**
- Consumes: `roqr::rtmp::Listener`/`ServerSession`/`SessionEvents`/`RtmpMessage`, `roqr::rtmp::classify` (rtmp); `roqr::quic::Client`/`DeliveryMode` (quic); `to_frame`, command builders (Tasks 1-2).
- Produces:

```cpp
namespace roqr::gateway {
struct IngestOptions {
    uint16_t rtmp_port = 1935;
    std::string roqr_host = "127.0.0.1";
    uint16_t roqr_port = 4443;
    bool insecure_skip_verify = true;
};
// Accepts one RTMP publisher on rtmp_port, re-originates its session over a
// RoQR connection to the server, and forwards media. One publisher at a
// time (gateway-grade). start() returns after the RTMP listener is up.
class IngestGateway {
public:
    IngestGateway();
    ~IngestGateway();  // stop()
    bool start(const IngestOptions& options);
    void stop();
    // True once an RTMP publisher connected AND the RoQR publish handshake
    // completed. For tests.
    bool wait_publishing(std::chrono::milliseconds timeout);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
```

  Behavior: the RTMP `ServerSession` answers the publisher's connect/createStream/publish locally (Plan 3). On `on_stream(name, publishing=true)`, ingest connects a `roqr::quic::Client` to the server, sends `build_connect`/`build_create_stream`/`build_publish(name)` as RoQR frames (flow 0, Stream), and marks itself publishing. On `on_message(media)`: convert to a `Frame` via `to_frame`, choose `DeliveryMode` by `classify` (sequence headers/metadata/control -> Stream; audio/video coded -> Auto), and `Client::send`. Empty-payload messages are dropped (RoQR requires payload > 0).

- [ ] **Step 1: Write the failing test `tests/integration/ingest_test.cpp`**

Uses our own RTMP client (HandshakeInitiator + ChunkWriter, as in the Plan 3 server_session_test) to publish into ingest, and a `Mode::Media` relay to receive; asserts the relay-side subscriber gets the media. Because that is a large harness, the test instead asserts the simpler contract: an ingest in front of a `Mode::Media` relay, with a RoQR `Client` subscriber playing "cam", receives the video frames the RTMP publisher sent.

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

#include "roqr/gateway/bridge.hpp"
#include "roqr/gateway/ingest.hpp"
#include "roqr/gateway/rtmp_commands.hpp"
#include "roqr/quic/client.hpp"
#include "roqr/relayd/server.hpp"
#include "roqr/rtmp/chunk_writer.hpp"
#include "roqr/rtmp/handshake.hpp"

using namespace std::chrono_literals;
using roqr::gateway::to_frame;

namespace {
const std::string kCertDir = ROQR_TEST_CERT_DIR;

// Minimal RTMP publisher client (drives ingest's RTMP listener).
struct RtmpPublisher {
    int fd = -1;
    roqr::rtmp::HandshakeInitiator hs;
    roqr::rtmp::ChunkWriter writer;

    bool connect_and_publish(uint16_t port, const std::string& name) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)))
            return false;
        if (!send_all(hs.start())) return false;
        uint8_t buf[4096];
        std::vector<uint8_t> c2;
        while (!hs.done()) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            if (!hs.feed(std::span<const uint8_t>(buf, size_t(n)), c2))
                return false;
            if (!c2.empty()) { send_all(c2); c2.clear(); }
        }
        send_cmd(roqr::gateway::build_connect(1, "live", "rtmp://h/live"));
        send_cmd(roqr::gateway::build_create_stream(2));
        send_cmd(roqr::gateway::build_publish(3, name));
        return true;
    }
    void send_cmd(const roqr::rtmp::RtmpMessage& m) {
        std::vector<uint8_t> wire;
        writer.write(m, wire);
        send_all(wire);
    }
    void send_video(uint32_t ts, std::vector<uint8_t> payload) {
        roqr::rtmp::RtmpMessage m;
        m.type = 9;
        m.timestamp = ts;
        m.message_stream_id = 1;
        m.chunk_stream_id = 6;
        m.payload = std::move(payload);
        std::vector<uint8_t> wire;
        writer.write(m, wire);
        send_all(wire);
    }
    bool send_all(const std::vector<uint8_t>& d) {
        size_t off = 0;
        while (off < d.size()) {
            ssize_t n = ::send(fd, d.data() + off, d.size() - off, 0);
            if (n <= 0) return false;
            off += size_t(n);
        }
        return true;
    }
    ~RtmpPublisher() { if (fd >= 0) ::close(fd); }
};

struct Collector {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<roqr::Frame> frames;
    void add(const roqr::Frame& f) {
        std::lock_guard lock(mutex);
        frames.push_back(f);
        cv.notify_all();
    }
    bool wait_video(std::chrono::milliseconds t) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, t, [&] {
            for (const auto& f : frames)
                if (f.message_type == 9) return true;
            return false;
        });
    }
};
}  // namespace

TEST_CASE("ingest bridges an RTMP publisher onto a RoQR relay") {
    roqr::relayd::Server relay;
    roqr::relayd::ServerOptions ro;
    ro.port = 45582;
    ro.cert_file = kCertDir + "/cert.pem";
    ro.key_file = kCertDir + "/key.pem";
    ro.mode = roqr::relayd::Mode::Media;
    REQUIRE(relay.start(ro));

    // Subscriber plays "cam" from the relay.
    Collector got;
    roqr::quic::Client sub;
    sub.on_message([&](const roqr::Frame& f) { got.add(f); });
    REQUIRE(sub.connect("127.0.0.1", 45582));
    REQUIRE(sub.wait_connected(5s));
    sub.send(to_frame(roqr::gateway::build_connect(1, "live", "rtmp://h"), 0),
             roqr::quic::DeliveryMode::Stream);
    sub.send(to_frame(roqr::gateway::build_create_stream(2), 0),
             roqr::quic::DeliveryMode::Stream);
    sub.send(to_frame(roqr::gateway::build_play(3, "cam"), 0),
             roqr::quic::DeliveryMode::Stream);

    // Ingest in front of the relay.
    roqr::gateway::IngestGateway ingest;
    roqr::gateway::IngestOptions io;
    io.rtmp_port = 45583;
    io.roqr_host = "127.0.0.1";
    io.roqr_port = 45582;
    REQUIRE(ingest.start(io));

    RtmpPublisher pub;
    REQUIRE(pub.connect_and_publish(45583, "cam"));
    REQUIRE(ingest.wait_publishing(5s));
    pub.send_video(0, {0x17, 0x00, 0x11});   // seq header
    pub.send_video(40, {0x17, 0x01, 0x22});  // keyframe

    REQUIRE(got.wait_video(5s));
    ingest.stop();
    sub.close();
    sub.wait_closed(5s);
    relay.stop();
}
```

Add `integration/ingest_test.cpp` to `roqr-integration-tests`.

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to compile (missing `roqr/gateway/ingest.hpp`).

- [ ] **Step 3: Implement `gateway/include/roqr/gateway/ingest.hpp`** — the interface block above with `#pragma once` and includes (`<chrono>`, `<cstdint>`, `<memory>`, `<string>`).

- [ ] **Step 4: Implement `gateway/src/ingest.cpp`**

```cpp
#include "roqr/gateway/ingest.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "roqr/gateway/bridge.hpp"
#include "roqr/gateway/rtmp_commands.hpp"
#include "roqr/quic/client.hpp"
#include "roqr/rtmp/classify.hpp"
#include "roqr/rtmp/server_session.hpp"

namespace roqr::gateway {

struct IngestGateway::Impl {
    IngestOptions options;
    roqr::rtmp::Listener listener;
    roqr::quic::Client client;
    std::mutex mutex;
    std::condition_variable cv;
    bool publishing = false;

    roqr::quic::DeliveryMode mode_for(const roqr::rtmp::RtmpMessage& msg) {
        if (msg.type != 8 && msg.type != 9) {
            return roqr::quic::DeliveryMode::Stream;  // commands, metadata
        }
        const auto info = msg.type == 9
                              ? roqr::rtmp::classify_video(msg.payload)
                              : roqr::rtmp::classify_audio(msg.payload);
        if (info.force_stream ||
            info.cls == roqr::rtmp::MediaClass::SequenceHeader ||
            info.cls == roqr::rtmp::MediaClass::Metadata ||
            info.cls == roqr::rtmp::MediaClass::Control) {
            return roqr::quic::DeliveryMode::Stream;
        }
        return roqr::quic::DeliveryMode::Auto;
    }

    void begin_publish(const std::string& name) {
        if (!client.connect(options.roqr_host, options.roqr_port,
                            [&] {
                                roqr::quic::ClientOptions o;
                                o.insecure_skip_verify =
                                    options.insecure_skip_verify;
                                return o;
                            }())) {
            return;
        }
        if (!client.wait_connected(std::chrono::seconds(5))) return;
        client.send(to_frame(build_connect(1, "live", "rtmp://roqr/live"), 0),
                    roqr::quic::DeliveryMode::Stream);
        client.send(to_frame(build_create_stream(2), 0),
                    roqr::quic::DeliveryMode::Stream);
        client.send(to_frame(build_publish(3, name), 0),
                    roqr::quic::DeliveryMode::Stream);
        {
            std::lock_guard lock(mutex);
            publishing = true;
        }
        cv.notify_all();
    }

    void forward(const roqr::rtmp::RtmpMessage& msg) {
        if (msg.payload.empty()) return;  // RoQR requires payload > 0
        client.send(to_frame(msg, 0), mode_for(msg));
    }
};

IngestGateway::IngestGateway() : impl_(std::make_unique<Impl>()) {}
IngestGateway::~IngestGateway() { stop(); }

bool IngestGateway::start(const IngestOptions& options) {
    impl_->options = options;
    Impl* impl = impl_.get();
    return impl_->listener.start(
        options.rtmp_port,
        [impl](roqr::rtmp::ServerSession&) {
            roqr::rtmp::SessionEvents e;
            e.on_stream = [impl](roqr::rtmp::ServerSession&,
                                 const std::string& name, bool publishing) {
                if (publishing) impl->begin_publish(name);
            };
            e.on_message = [impl](roqr::rtmp::ServerSession&,
                                  const roqr::rtmp::RtmpMessage& msg) {
                impl->forward(msg);
            };
            return e;
        });
}

void IngestGateway::stop() {
    impl_->listener.stop();
    impl_->client.close();
    impl_->client.wait_closed(std::chrono::seconds(2));
}

bool IngestGateway::wait_publishing(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->cv.wait_for(lock, timeout, [&] { return impl_->publishing; });
}

}  // namespace roqr::gateway
```

Add `src/ingest.cpp` to `gateway/CMakeLists.txt` and link `roqr-rtmp` (already linked) — no new link needed. Add the test file to `roqr-integration-tests`.

- [ ] **Step 5: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 128 prior + 1 new = 129. `--repeat until-fail:2` for stability (the RTMP handshake + RoQR handshake chain is timing-sensitive; the wait helpers bound it).

- [ ] **Step 6: Commit**

```bash
git add gateway tests
git commit -m "Add ingest gateway bridging RTMP publish to RoQR"
```

---

### Task 6: Egress gateway (RoQR -> RTMP play) with gap recovery

**Files:**
- Create: `gateway/include/roqr/gateway/gap.hpp`
- Create: `gateway/include/roqr/gateway/egress.hpp`
- Create: `gateway/src/egress.cpp`
- Modify: `gateway/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/integration/egress_test.cpp`

**Interfaces:**
- Consumes: `roqr::quic::Client`, `roqr::rtmp::Listener`/`ServerSession`, `to_rtmp`, command builders, `roqr::rtmp::classify`.
- Produces (header-only, used by egress and unit-tested in Task 7): `class roqr::gateway::GapTracker` with `static constexpr uint32_t kJumpThreshold = 5000;` and `bool accept(uint32_t timestamp, roqr::rtmp::MediaClass cls)` — per-flow video continuity per draft s8 (returns whether to deliver the frame).
- Produces:

```cpp
namespace roqr::gateway {
struct EgressOptions {
    uint16_t rtmp_port = 1936;
    std::string roqr_host = "127.0.0.1";
    uint16_t roqr_port = 4443;
    std::string stream_name = "cam";
    bool insecure_skip_verify = true;
};
// Accepts one RTMP player (ffplay) on rtmp_port, connects to the RoQR
// server, plays stream_name, and serves received media to the player.
// Applies draft s8 gap recovery: after a suspected datagram gap, drops
// non-keyframe video until the next keyframe/sequence header.
class EgressGateway {
public:
    EgressGateway();
    ~EgressGateway();
    bool start(const EgressOptions& options);
    void stop();
    bool wait_playing(std::chrono::milliseconds timeout);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
```

  Gap recovery: track the last delivered video timestamp per flow. On a received video frame, if the timeline is marked discontinuous (initially false; set true on a timestamp regression or a jump larger than a threshold, e.g. 5000 ms), drop it unless `classify_video` reports `Keyframe` or `SequenceHeader`; a keyframe/seq-header clears the discontinuity. Audio and metadata always pass. Commands (`_result`/`onStatus`) from the relay are consumed, not forwarded to the player (the player has its own local ServerSession replies).

- [ ] **Step 1: Write the failing test `tests/integration/egress_test.cpp`**

Drives: a `Mode::Media` relay with a publisher (a RoQR `Client` sending video), an `EgressGateway` playing that stream, and an RTMP player (HandshakeInitiator + ChunkReader) that connects to egress and receives the video. Asserts the player receives the video frame.

```cpp
#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "roqr/gateway/bridge.hpp"
#include "roqr/gateway/egress.hpp"
#include "roqr/gateway/rtmp_commands.hpp"
#include "roqr/quic/client.hpp"
#include "roqr/relayd/server.hpp"
#include "roqr/rtmp/chunk_reader.hpp"
#include "roqr/rtmp/chunk_writer.hpp"
#include "roqr/rtmp/handshake.hpp"

using namespace std::chrono_literals;
using roqr::gateway::to_frame;

namespace {
const std::string kCertDir = ROQR_TEST_CERT_DIR;

// Minimal RTMP player (drives egress's RTMP listener, receives media).
struct RtmpPlayer {
    int fd = -1;
    roqr::rtmp::HandshakeInitiator hs;
    roqr::rtmp::ChunkReader reader;
    roqr::rtmp::ChunkWriter writer;

    bool connect_and_play(uint16_t port, const std::string& name) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)))
            return false;
        send_all(hs.start());
        uint8_t buf[4096];
        std::vector<uint8_t> c2;
        while (!hs.done()) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            hs.feed(std::span<const uint8_t>(buf, size_t(n)), c2);
            if (!c2.empty()) { send_all(c2); c2.clear(); }
        }
        send_cmd(roqr::gateway::build_connect(1, "live", "rtmp://h/live"));
        send_cmd(roqr::gateway::build_create_stream(2));
        send_cmd(roqr::gateway::build_play(3, name));
        return true;
    }
    void send_cmd(const roqr::rtmp::RtmpMessage& m) {
        std::vector<uint8_t> wire;
        writer.write(m, wire);
        send_all(wire);
    }
    bool wait_video(std::chrono::milliseconds t) {
        uint8_t buf[4096];
        auto deadline = std::chrono::steady_clock::now() + t;
        while (std::chrono::steady_clock::now() < deadline) {
            while (auto m = reader.next()) {
                if (m->type == 9) return true;
            }
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            reader.feed(std::span<const uint8_t>(buf, size_t(n)));
        }
        return false;
    }
    bool send_all(const std::vector<uint8_t>& d) {
        size_t off = 0;
        while (off < d.size()) {
            ssize_t n = ::send(fd, d.data() + off, d.size() - off, 0);
            if (n <= 0) return false;
            off += size_t(n);
        }
        return true;
    }
    ~RtmpPlayer() { if (fd >= 0) ::close(fd); }
};

roqr::rtmp::RtmpMessage vid(uint32_t ts, std::vector<uint8_t> p) {
    roqr::rtmp::RtmpMessage m;
    m.type = 9;
    m.timestamp = ts;
    m.message_stream_id = 1;
    m.chunk_stream_id = 6;
    m.payload = std::move(p);
    return m;
}
}  // namespace

TEST_CASE("egress plays a RoQR stream out to an RTMP player") {
    roqr::relayd::Server relay;
    roqr::relayd::ServerOptions ro;
    ro.port = 45584;
    ro.cert_file = kCertDir + "/cert.pem";
    ro.key_file = kCertDir + "/key.pem";
    ro.mode = roqr::relayd::Mode::Media;
    REQUIRE(relay.start(ro));

    // Publisher into the relay.
    roqr::quic::Client pub;
    REQUIRE(pub.connect("127.0.0.1", 45584));
    REQUIRE(pub.wait_connected(5s));
    pub.send(to_frame(roqr::gateway::build_connect(1, "live", "rtmp://h"), 0),
             roqr::quic::DeliveryMode::Stream);
    pub.send(to_frame(roqr::gateway::build_create_stream(2), 0),
             roqr::quic::DeliveryMode::Stream);
    pub.send(to_frame(roqr::gateway::build_publish(3, "cam"), 0),
             roqr::quic::DeliveryMode::Stream);
    std::this_thread::sleep_for(200ms);

    // Egress plays "cam" and serves it to an RTMP player.
    roqr::gateway::EgressGateway egress;
    roqr::gateway::EgressOptions eo;
    eo.rtmp_port = 45585;
    eo.roqr_host = "127.0.0.1";
    eo.roqr_port = 45584;
    eo.stream_name = "cam";
    REQUIRE(egress.start(eo));
    REQUIRE(egress.wait_playing(5s));

    RtmpPlayer player;
    REQUIRE(player.connect_and_play(45585, "cam"));
    std::this_thread::sleep_for(200ms);

    // Publish a seq header + keyframe; the player must receive video.
    pub.send(to_frame(vid(0, {0x17, 0x00, 0x11}), 0),
             roqr::quic::DeliveryMode::Stream);
    pub.send(to_frame(vid(40, {0x17, 0x01, 0x22}), 0),
             roqr::quic::DeliveryMode::Stream);

    REQUIRE(player.wait_video(5s));
    egress.stop();
    pub.close();
    pub.wait_closed(5s);
    relay.stop();
}
```

Add `integration/egress_test.cpp` to `roqr-integration-tests`.

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to compile (missing `roqr/gateway/egress.hpp`).

- [ ] **Step 3: Create `gateway/include/roqr/gateway/gap.hpp`** (the draft s8 continuity tracker; egress and the Task 7 unit test both use it)

```cpp
#pragma once

#include <cstdint>

#include "roqr/rtmp/classify.hpp"

namespace roqr::gateway {

// Per-flow video continuity tracker (draft s8). Feed each video frame's
// timestamp and its MediaClass; returns whether to deliver it. On a
// suspected gap (timestamp regression or a jump past kJumpThreshold) it
// drops non-recovery frames until the next keyframe or sequence header.
class GapTracker {
public:
    static constexpr uint32_t kJumpThreshold = 5000;  // ms

    bool accept(uint32_t timestamp, roqr::rtmp::MediaClass cls) {
        const bool recover = cls == roqr::rtmp::MediaClass::Keyframe ||
                             cls == roqr::rtmp::MediaClass::SequenceHeader;
        if (have_) {
            const bool regressed = timestamp < last_ts_;
            const bool jumped = timestamp > last_ts_ + kJumpThreshold;
            if (regressed || jumped) discontinuous_ = true;
        }
        have_ = true;
        last_ts_ = timestamp;
        if (discontinuous_ && !recover) return false;
        if (recover) discontinuous_ = false;
        return true;
    }

private:
    bool discontinuous_ = false;
    bool have_ = false;
    uint32_t last_ts_ = 0;
};

}  // namespace roqr::gateway
```

- [ ] **Step 4: Implement `gateway/include/roqr/gateway/egress.hpp`** — the interface block with `#pragma once` and includes (`<chrono>`, `<cstdint>`, `<memory>`, `<string>`).

- [ ] **Step 5: Implement `gateway/src/egress.cpp`**

```cpp
#include "roqr/gateway/egress.hpp"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "roqr/gateway/bridge.hpp"
#include "roqr/gateway/gap.hpp"
#include "roqr/gateway/rtmp_commands.hpp"
#include "roqr/quic/client.hpp"
#include "roqr/rtmp/classify.hpp"
#include "roqr/rtmp/server_session.hpp"

namespace roqr::gateway {

struct EgressGateway::Impl {
    EgressOptions options;
    roqr::quic::Client client;
    roqr::rtmp::Listener listener;

    std::mutex mutex;
    std::condition_variable cv;
    bool playing = false;

    // Player session + init-frame cache, both guarded by player_mutex.
    // player_ready gates live delivery: the RTMP handshake and play must
    // complete (on_stream fires) before we write media to the fd.
    std::mutex player_mutex;
    roqr::rtmp::ServerSession* player = nullptr;
    bool player_ready = false;
    // Latest metadata + audio/video sequence headers, by message type, in
    // insertion order — replayed to a player when it starts playing so a
    // late-joining ffplay can decode even though play() was sent to the
    // relay before the player connected.
    std::vector<uint8_t> init_types;
    std::map<uint8_t, roqr::rtmp::RtmpMessage> init_cache;

    roqr::gateway::GapTracker gaps;  // draft s8, defined in gap.hpp

    static bool is_init(const roqr::rtmp::RtmpMessage& msg) {
        if (msg.type == 18 || msg.type == 15) return true;
        if (msg.type == 8 || msg.type == 9) {
            const auto info = msg.type == 9
                                  ? roqr::rtmp::classify_video(msg.payload)
                                  : roqr::rtmp::classify_audio(msg.payload);
            return info.cls == roqr::rtmp::MediaClass::SequenceHeader;
        }
        return false;
    }

    bool accept_video(const roqr::rtmp::RtmpMessage& msg) {
        return gaps.accept(msg.timestamp,
                          roqr::rtmp::classify_video(msg.payload).cls);
    }

    void on_frame(const roqr::Frame& f) {
        roqr::rtmp::RtmpMessage msg;
        if (!to_rtmp(f, msg)) return;
        if (msg.type == roqr::rtmp::kTypeCommandAmf0) return;  // relay replies
        if (msg.type == 9 && !accept_video(msg)) return;

        std::lock_guard lock(player_mutex);
        if (is_init(msg)) {
            if (init_cache.find(msg.type) == init_cache.end()) {
                init_types.push_back(msg.type);
            }
            init_cache[msg.type] = msg;
        }
        if (player != nullptr && player_ready) player->send(msg);
    }

    // Called on the session thread when the player issues play (after the
    // RTMP handshake). Primes it with the cached init frames, then opens
    // live delivery.
    void on_player_play() {
        std::lock_guard lock(player_mutex);
        if (player == nullptr) return;
        for (uint8_t type : init_types) player->send(init_cache.at(type));
        player_ready = true;
    }

    void begin_play() {
        roqr::quic::ClientOptions o;
        o.insecure_skip_verify = options.insecure_skip_verify;
        client.on_message([this](const roqr::Frame& f) { on_frame(f); });
        if (!client.connect(options.roqr_host, options.roqr_port, o)) return;
        if (!client.wait_connected(std::chrono::seconds(5))) return;
        client.send(to_frame(build_connect(1, "live", "rtmp://roqr/live"), 0),
                    roqr::quic::DeliveryMode::Stream);
        client.send(to_frame(build_create_stream(2), 0),
                    roqr::quic::DeliveryMode::Stream);
        client.send(to_frame(build_play(3, options.stream_name), 0),
                    roqr::quic::DeliveryMode::Stream);
        {
            std::lock_guard lock(mutex);
            playing = true;
        }
        cv.notify_all();
    }
};

EgressGateway::EgressGateway() : impl_(std::make_unique<Impl>()) {}
EgressGateway::~EgressGateway() { stop(); }

bool EgressGateway::start(const EgressOptions& options) {
    impl_->options = options;
    Impl* impl = impl_.get();
    impl->begin_play();  // connect + play before accepting the player
    return impl->listener.start(
        options.rtmp_port,
        [impl](roqr::rtmp::ServerSession& s) {
            {
                std::lock_guard lock(impl->player_mutex);
                impl->player = &s;
                impl->player_ready = false;
            }
            roqr::rtmp::SessionEvents e;
            e.on_stream = [impl](roqr::rtmp::ServerSession&,
                                 const std::string&, bool publishing) {
                if (!publishing) impl->on_player_play();  // a play request
            };
            e.on_close = [impl](roqr::rtmp::ServerSession&) {
                std::lock_guard lock(impl->player_mutex);
                impl->player = nullptr;
                impl->player_ready = false;
            };
            return e;
        });
}

void EgressGateway::stop() {
    impl_->listener.stop();
    impl_->client.close();
    impl_->client.wait_closed(std::chrono::seconds(2));
}

bool EgressGateway::wait_playing(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->cv.wait_for(lock, timeout, [&] { return impl_->playing; });
}

}  // namespace roqr::gateway
```

Add `src/egress.cpp` to `gateway/CMakeLists.txt`; add the test to `roqr-integration-tests`.

- [ ] **Step 6: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 129 prior + 1 new = 130. `--repeat until-fail:2` stable.

- [ ] **Step 7: Commit**

```bash
git add gateway tests
git commit -m "Add egress gateway with datagram gap recovery"
```

---

### Task 7: Example binaries and gap-recovery unit test

**Files:**
- Create: `examples/ingest_main.cpp`, `examples/egress_main.cpp`, `examples/duplex_main.cpp` (the relayd binary already exists from Plan 2)
- Modify: `examples/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Test: `tests/gateway/gap_recovery_test.cpp`

**Interfaces:**
- Consumes: `IngestGateway`/`EgressGateway` (Tasks 5-6), `roqr::gateway::GapTracker` (`gap.hpp`, created in Task 6).
- Produces: three CLI binaries and a focused unit test for the `GapTracker` policy the egress already uses.

- [ ] **Step 1: Write the failing test `tests/gateway/gap_recovery_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include "roqr/gateway/gap.hpp"

using namespace roqr::gateway;
using roqr::rtmp::MediaClass;

TEST_CASE("continuous stream delivers everything") {
    GapTracker g;
    CHECK(g.accept(0, MediaClass::SequenceHeader));
    CHECK(g.accept(40, MediaClass::Keyframe));
    CHECK(g.accept(80, MediaClass::Coded));
    CHECK(g.accept(120, MediaClass::Coded));
}

TEST_CASE("a forward jump drops coded frames until a keyframe") {
    GapTracker g;
    CHECK(g.accept(0, MediaClass::Keyframe));
    CHECK(g.accept(40, MediaClass::Coded));
    // Jump well past the threshold: discontinuity.
    CHECK_FALSE(g.accept(40 + GapTracker::kJumpThreshold + 1,
                         MediaClass::Coded));
    // Still dropping non-recovery frames.
    CHECK_FALSE(g.accept(10000, MediaClass::Coded));
    // A keyframe recovers the timeline.
    CHECK(g.accept(10040, MediaClass::Keyframe));
    CHECK(g.accept(10080, MediaClass::Coded));
}

TEST_CASE("a timestamp regression triggers recovery") {
    GapTracker g;
    CHECK(g.accept(1000, MediaClass::Keyframe));
    CHECK(g.accept(1040, MediaClass::Coded));
    CHECK_FALSE(g.accept(500, MediaClass::Coded));  // regression
    CHECK(g.accept(1080, MediaClass::SequenceHeader));  // recovers
    CHECK(g.accept(1120, MediaClass::Coded));
}
```

Add `gateway/gap_recovery_test.cpp` to `roqr-gateway-tests`.

- [ ] **Step 2: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — `gap.hpp` already exists from Task 6, so these three cases pass immediately (they pin the policy the egress relies on). If any fails, the GapTracker logic and the egress that shares it are both wrong — fix `gap.hpp`.

- [ ] **Step 3: Create the example binaries**

`examples/ingest_main.cpp`:

```cpp
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "roqr/gateway/ingest.hpp"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }
}  // namespace

int main(int argc, char** argv) {
    roqr::gateway::IngestOptions o;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--rtmp-port") && i + 1 < argc) {
            o.rtmp_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--roqr-host") && i + 1 < argc) {
            o.roqr_host = argv[++i];
        } else if (!std::strcmp(argv[i], "--roqr-port") && i + 1 < argc) {
            o.roqr_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else {
            std::fprintf(stderr,
                         "usage: roqr-ingest [--rtmp-port P] "
                         "[--roqr-host H] [--roqr-port P]\n");
            return 2;
        }
    }
    roqr::gateway::IngestGateway g;
    if (!g.start(o)) {
        std::fprintf(stderr, "ingest: failed to start\n");
        return 1;
    }
    std::printf("roqr-ingest: RTMP :%u -> RoQR %s:%u\n", o.rtmp_port,
                o.roqr_host.c_str(), o.roqr_port);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (!g_stop) {
        struct timespec ts {0, 200'000'000};
        nanosleep(&ts, nullptr);
    }
    g.stop();
    return 0;
}
```

`examples/egress_main.cpp` — the same shape driving `EgressGateway`/`EgressOptions` (flags `--rtmp-port`, `--roqr-host`, `--roqr-port`, `--stream`).

`examples/duplex_main.cpp` — starts an `IngestGateway` and an `EgressGateway` in one process pointed at the same RoQR server (demonstrating publish + subscribe over one deployment); flags for both port sets. Full source:

```cpp
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "roqr/gateway/egress.hpp"
#include "roqr/gateway/ingest.hpp"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }
}  // namespace

int main(int argc, char** argv) {
    roqr::gateway::IngestOptions in;
    roqr::gateway::EgressOptions eg;
    in.rtmp_port = 1935;
    eg.rtmp_port = 1936;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--roqr-host") && i + 1 < argc) {
            in.roqr_host = eg.roqr_host = argv[++i];
        } else if (!std::strcmp(argv[i], "--roqr-port") && i + 1 < argc) {
            in.roqr_port = eg.roqr_port =
                static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--stream") && i + 1 < argc) {
            eg.stream_name = argv[++i];
        } else {
            std::fprintf(stderr,
                         "usage: roqr-duplex [--roqr-host H] "
                         "[--roqr-port P] [--stream NAME]\n");
            return 2;
        }
    }
    roqr::gateway::IngestGateway ingest;
    roqr::gateway::EgressGateway egress;
    if (!ingest.start(in) || !egress.start(eg)) {
        std::fprintf(stderr, "duplex: failed to start\n");
        return 1;
    }
    std::printf("roqr-duplex: ingest RTMP :%u, egress RTMP :%u, RoQR %s:%u\n",
                in.rtmp_port, eg.rtmp_port, in.roqr_host.c_str(),
                in.roqr_port);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (!g_stop) {
        struct timespec ts {0, 200'000'000};
        nanosleep(&ts, nullptr);
    }
    ingest.stop();
    egress.stop();
    return 0;
}
```

`examples/CMakeLists.txt`:

```cmake
add_executable(roqr-ingest ingest_main.cpp)
target_link_libraries(roqr-ingest PRIVATE roqr-gateway)

add_executable(roqr-egress egress_main.cpp)
target_link_libraries(roqr-egress PRIVATE roqr-gateway)

add_executable(roqr-duplex duplex_main.cpp)
target_link_libraries(roqr-duplex PRIVATE roqr-gateway)

foreach(t roqr-ingest roqr-egress roqr-duplex)
  target_compile_options(${t} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
endforeach()
```

- [ ] **Step 4: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 130 prior + 3 new = 133. The three example binaries link and a bad arg returns 2. Verify manually:
```bash
./build/dev/examples/roqr-ingest --bogus; echo "exit=$?"   # expect usage + exit=2
```

- [ ] **Step 5: Commit**

```bash
git add gateway examples tests
git commit -m "Add example binaries and gap-recovery unit test"
```

---

### Task 8: ffmpeg end-to-end test script

**Files:**
- Create: `tests/e2e/run_ffmpeg_e2e.sh` (executable)
- Create: `tests/e2e/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt` (add the e2e subdir under ROQR_BUILD_EXAMPLES)

**Interfaces:**
- Consumes: the built `roqr-relayd`, `roqr-ingest`, `roqr-egress` binaries and the test cert fixture.
- Produces: a CTest test `roqr-ffmpeg-e2e` that publishes a real ffmpeg stream through ingest -> relayd -> egress and pulls it back with ffmpeg, asserting the output is a valid, non-empty media file with the expected codec. Self-skips (exit 0) when ffmpeg/ffprobe are absent.

- [ ] **Step 1: Create `tests/e2e/run_ffmpeg_e2e.sh`**

```bash
#!/usr/bin/env bash
# End-to-end: ffmpeg publish -> roqr-ingest -> roqr-relayd -> roqr-egress
# -> ffmpeg pull. Self-skips when ffmpeg/ffprobe are unavailable.
#
# Args: $1 = build bin dir (contains roqr-relayd/ingest/egress),
#       $2 = cert dir (cert.pem/key.pem), $3 = codec: "h264" or "hevc".
set -uo pipefail

BIN="$1"; CERTS="$2"; CODEC="${3:-h264}"

if ! command -v ffmpeg >/dev/null || ! command -v ffprobe >/dev/null; then
    echo "SKIP: ffmpeg/ffprobe not found on PATH"
    exit 0
fi

RELAY_PORT=45590
INGEST_RTMP=45591
EGRESS_RTMP=45592
STREAM="cam"
WORK="$(mktemp -d)"
OUT="${WORK}/out.flv"
PIDS=()
cleanup() {
    for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "${WORK}"
}
trap cleanup EXIT

case "$CODEC" in
    h264) VENC=(-c:v libx264 -preset ultrafast); PROBE="h264";;
    hevc)
        if ! ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx265; then
            echo "SKIP: libx265 not available for the HEVC/E-RTMP case"
            exit 0
        fi
        VENC=(-c:v libx265 -tag:v hvc1); PROBE="hevc";;
    *) echo "unknown codec $CODEC"; exit 2;;
esac

"${BIN}/roqr-relayd" --cert "${CERTS}/cert.pem" --key "${CERTS}/key.pem" \
    --port "${RELAY_PORT}" --mode media &
PIDS+=($!)
sleep 0.5

"${BIN}/roqr-egress" --rtmp-port "${EGRESS_RTMP}" \
    --roqr-host 127.0.0.1 --roqr-port "${RELAY_PORT}" --stream "${STREAM}" &
PIDS+=($!)
"${BIN}/roqr-ingest" --rtmp-port "${INGEST_RTMP}" \
    --roqr-host 127.0.0.1 --roqr-port "${RELAY_PORT}" &
PIDS+=($!)
sleep 1

# Pull from egress into a file (runs in the background, bounded by -t).
ffmpeg -hide_banner -loglevel error -y \
    -i "rtmp://127.0.0.1:${EGRESS_RTMP}/live/${STREAM}" \
    -t 3 -c copy "${OUT}" &
PULL_PID=$!
sleep 0.5

# Publish a 3s synthetic stream into ingest.
ffmpeg -hide_banner -loglevel error \
    -re -f lavfi -i "testsrc2=size=320x240:rate=15" -t 3 \
    "${VENC[@]}" -pix_fmt yuv420p -f flv \
    "rtmp://127.0.0.1:${INGEST_RTMP}/live/${STREAM}"

wait "${PULL_PID}"

if [ ! -s "${OUT}" ]; then
    echo "FAIL: egress produced no output"
    exit 1
fi
CODEC_SEEN="$(ffprobe -hide_banner -loglevel error \
    -select_streams v:0 -show_entries stream=codec_name \
    -of default=nw=1:nk=1 "${OUT}")"
echo "e2e output codec: ${CODEC_SEEN} (expected ${PROBE})"
if [ "${CODEC_SEEN}" != "${PROBE}" ]; then
    echo "FAIL: expected ${PROBE}, got ${CODEC_SEEN}"
    exit 1
fi
echo "PASS: ${CODEC} end-to-end through ingest -> relayd -> egress"
exit 0
```

- [ ] **Step 2: Create `tests/e2e/CMakeLists.txt`**

```cmake
# The e2e script needs the three binaries and the cert fixture. It is a
# plain CTest test that self-skips without ffmpeg.
add_test(
  NAME roqr-ffmpeg-e2e-h264
  COMMAND ${CMAKE_COMMAND} -E env
    ${CMAKE_CURRENT_SOURCE_DIR}/run_ffmpeg_e2e.sh
    $<TARGET_FILE_DIR:roqr-relayd> ${ROQR_TEST_CERT_DIR} h264)
set_tests_properties(roqr-ffmpeg-e2e-h264 PROPERTIES TIMEOUT 60)

add_test(
  NAME roqr-ffmpeg-e2e-hevc
  COMMAND ${CMAKE_COMMAND} -E env
    ${CMAKE_CURRENT_SOURCE_DIR}/run_ffmpeg_e2e.sh
    $<TARGET_FILE_DIR:roqr-relayd> ${ROQR_TEST_CERT_DIR} hevc)
set_tests_properties(roqr-ffmpeg-e2e-hevc PROPERTIES TIMEOUT 60)
```

Make the script executable and add the subdir in `tests/CMakeLists.txt` inside the `ROQR_BUILD_EXAMPLES` block (after the gateway tests, and it needs the cert fixture, so it must be inside the `ROQR_BUILD_TOOLS` guard where `ROQR_TEST_CERT_DIR` is defined — place it there and depend on `roqr-testcerts`):

```cmake
  add_dependencies(roqr-relayd roqr-testcerts)  # ensure certs exist for e2e
  add_subdirectory(e2e)
```

- [ ] **Step 3: Run to verify**

Run: `chmod +x tests/e2e/run_ffmpeg_e2e.sh && cmake --preset dev && cmake --build --preset dev && ctest --preset dev -R roqr-ffmpeg-e2e --output-on-failure`
Expected: if ffmpeg is installed, both tests PASS (h264 real, hevc real or SKIP if libx265 absent); if ffmpeg is absent, both SKIP with exit 0 (CTest counts them as passed). Full suite `ctest --preset dev`: 133 + 2 e2e = 135 registered (the two e2e tests pass or skip).

If the e2e FAILS on a real ffmpeg (not a skip), the failure is a genuine gateway interop bug — capture the script output, diagnose against the ingest/egress logs, and record it before committing. Do not commit a red e2e.

- [ ] **Step 4: Commit**

```bash
git add tests/e2e tests/CMakeLists.txt
git commit -m "Add ffmpeg end-to-end test with h264 and hevc cases"
```

---

### Task 9: GitHub Actions CI

**Files:**
- Create: `.github/workflows/ci.yml`
- Create: `README.md` (project root — authorized: needed for the repo landing page; keep short)

**Interfaces:**
- Consumes: `scripts/setup_picoquic_deps.sh`, the CMake presets, the test suite.
- Produces: a CI workflow that builds and tests on push/PR: a core-only job (ROQR_BUILD_QUIC=OFF, fast) and a full job (deps + quic + rtmp + examples + ffmpeg e2e). A gcc and clang matrix on the full job.

- [ ] **Step 1: Create `.github/workflows/ci.yml`**

```yaml
name: CI

on:
  push:
    branches: [main]
  pull_request:

jobs:
  core:
    name: core-only (no picoquic)
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install build tools
        run: sudo apt-get update && sudo apt-get install -y cmake g++ ninja-build
      - name: Configure (core only)
        run: cmake -S . -B build -DROQR_BUILD_QUIC=OFF -DROQR_BUILD_RTMP=ON
             -DROQR_BUILD_EXAMPLES=OFF -DROQR_BUILD_TESTS=ON
      - name: Build
        run: cmake --build build --parallel
      - name: Test
        run: ctest --test-dir build --output-on-failure

  full:
    name: full (${{ matrix.cc }})
    runs-on: ubuntu-latest
    strategy:
      fail-fast: false
      matrix:
        include:
          - { cc: gcc, cxx: g++ }
          - { cc: clang, cxx: clang++ }
    steps:
      - uses: actions/checkout@v4
      - name: Install dependencies
        run: sudo apt-get update && sudo apt-get install -y
             cmake g++ clang ninja-build libssl-dev ffmpeg
      - name: Build picoquic deps
        run: eval "$(scripts/setup_picoquic_deps.sh)"; echo "$ROQR_PICOQUIC_SOURCE_DIR"
      - name: Configure
        env:
          CC: ${{ matrix.cc }}
          CXX: ${{ matrix.cxx }}
        run: |
          eval "$(scripts/setup_picoquic_deps.sh)"
          cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
            -DROQR_BUILD_QUIC=ON -DROQR_BUILD_RTMP=ON \
            -DROQR_BUILD_TOOLS=ON -DROQR_BUILD_EXAMPLES=ON \
            -DROQR_BUILD_TESTS=ON
      - name: Build
        run: cmake --build build --parallel
      - name: Test
        run: ctest --test-dir build --output-on-failure
```

(Note: `setup_picoquic_deps.sh` prints exports to stdout; the "Build picoquic deps" step primes the `.deps` clone/build so the "Configure" step's `eval` is fast. Each step is its own shell, so the `eval` is repeated where the env is needed.)

- [ ] **Step 2: Create a short `README.md`**

```markdown
# libroqr

A C++20 implementation of RoQR (RTMP over QUIC,
draft-gregoire-rtmp-over-quic) with a sans-I/O protocol core, a picoquic
transport, an RTMP/AMF gateway module, a test relay, and example gateways.

## Reference media path

```
ffmpeg (RTMP publish) -> roqr-ingest -> roqr-relayd -> roqr-egress -> ffmpeg (RTMP play)
```

RoQR carries RTMP message metadata and payloads over QUIC streams and
DATAGRAM frames.

## Build

```
eval "$(scripts/setup_picoquic_deps.sh)"   # clone + build pinned picoquic/picotls
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The core protocol library builds without picoquic:
`cmake -S . -B build -DROQR_BUILD_QUIC=OFF && cmake --build build`.

## Layout

- `core/` sans-I/O RoQR frame codec, flow table (no dependencies)
- `quic/` picoquic client transport
- `rtmp/` RTMP handshake, chunking, AMF0, E-RTMP media classifier
- `gateway/` RTMP<->RoQR bridge, ingest/egress gateways
- `tools/relayd/` the RoQR test relay
- `examples/` roqr-ingest, roqr-egress, roqr-duplex

License: Apache-2.0.
```

- [ ] **Step 3: Verify locally**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: full suite green (135). The workflow file is YAML-only; verify it parses (no tabs, valid structure) by eye. Do not attempt to run GitHub Actions locally.

- [ ] **Step 4: Commit**

```bash
git add .github README.md
git commit -m "Add GitHub Actions CI and project README"
```

---

## Completion Criteria

- `ctest --preset dev` green: 135 registered tests (114 baseline + 21 new: bridge 4, commands 5, router 3, gap 3, relay-media 2, ingest 1, egress 1, examples 0 + e2e 2), `--repeat until-fail:2` stable on the loopback integration tests, warning-clean.
- The three example binaries build and run; `roqr-relayd --mode media` serves the reference path.
- ffmpeg e2e passes on a machine with ffmpeg (h264 real; hevc real or skipped without libx265), skips cleanly without ffmpeg.
- CI builds core-only and full (gcc + clang) and runs the suite.
- Spec coverage delivered: RTMP<->RoQR bridge with narrowing guards, ingest classification-based delivery mode (s10), egress gap recovery (s8), relay stream-name routing + init-frame caching, the full ffmpeg reference path.

## Follow-On Plan

- Plan 5: C FFI (`roqr.h`, `roqr_rtmp.h`), JNI bindings (desktop + Android NDK), Java samples — wrapping the `Client` and the gateway/RTMP surfaces this plan exercised. Carry forward the Plan 2/3 deferred items (TSAN CI job; the known QUIC integration test #63 flake; ack seq-number wrap; Reader internal bounds asserts + fuzzing).
