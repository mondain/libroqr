# libroqr Plan 5: C FFI, JNI Bindings, and Java Samples Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A stable C ABI (`roqr.h` over the QUIC `Client`, `roqr_rtmp.h` over the ingest/egress gateways) plus JNI bindings (`org.red5.roqr`) for desktop JVM with an Android NDK build path, and Java sample apps — so non-C++ callers can publish and subscribe RTMP-over-QUIC.

**Architecture:** A `roqr-ffi` shared library exposes opaque-handle C functions over `roqr::quic::Client` and the `roqr::gateway::IngestGateway`/`EgressGateway`; frames cross the ABI as a plain C struct (pointer+length payload), callbacks as C function pointers with `user_data`. A `roqr-jni` shared library maps `org.red5.roqr` Java classes onto that C ABI; because native callbacks fire on the QUIC network thread, the JNI layer caches the `JavaVM*` and does `AttachCurrentThread`/`DetachCurrentThread` around every up-call, delivering to a Java listener held as a global ref. CMake builds the JNI lib via `FindJNI` and compiles the Java classes to a jar via `UseJava`; Android uses an NDK toolchain file (documented build, not in CI).

**Tech Stack:** C++20, C99 ABI headers, JNI (JDK 21 desktop), Catch2 v3 for C-side tests, JUnit-free Java smoke tests run via CTest, CMake `FindJNI`/`UseJava`, Android NDK (documented). No new third-party deps.

**Spec:** `docs/superpowers/specs/2026-07-04-libroqr-design.md` (FFI and JNI components). The FFI wraps the surfaces Plan 4 exercised.

## Global Constraints

- C ABI headers are C99-clean (compilable as C: `extern "C"` guards, no C++ types, fixed-width ints, opaque struct pointers). Header layout `ffi/include/roqr/roqr.h` and `ffi/include/roqr/roqr_rtmp.h`. C symbol prefix `roqr_`.
- `roqr-ffi` is a SHARED library; `roqr-jni` is a SHARED library. Namespaces stay C++ only inside `.cpp`. Java package `org.red5.roqr`. All new targets warning-clean under `$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>`.
- CMake option `ROQR_BUILD_FFI` (ON default; wraps the QUIC Client + the `roqr-gateway` lib, so it needs QUIC+RTMP — which build the gateway lib per Plan 4's decoupling; when QUIC or RTMP is OFF, FFI is skipped silently, NOT a hard error, so the core-only CI job keeps working) and `ROQR_BUILD_JNI` (OFF default; requires FFI and a JDK via `find_package(JNI)`; skip cleanly if JNI is absent so CI without a JDK still builds).
- **Callback contract (deferred Plan 4 API item, now the ABI contract):** native callbacks (`on_message`, `on_closed`) fire on the QUIC network thread. C callers MUST NOT block in the callback and MUST NOT call back into `roqr_*` send/close on that thread beyond the documented thread-safe ones (`send`/`close`/`bind_flow`/`retire_flow` are thread-safe; `destroy`/`wait_*` are NOT callback-safe). This is stated in the header doc comments verbatim.
- **JNI thread attachment:** every native->Java up-call attaches the current (network) thread to the JVM via `AttachCurrentThread`, invokes the listener through a cached method id + global ref, and detaches before returning. The `JavaVM*` is captured in `JNI_OnLoad`.
- **start() error surface (deferred Plan 4 API item):** the FFI gateway `..._start` returns a `roqr_error_t` (not bool) and readiness is a separate `..._wait_publishing`/`..._wait_playing` call; the header documents that `start` returning `ROQR_OK` means the RTMP listener is up, NOT that the RoQR server leg is live — callers must wait for readiness.
- Deferred-from-Plan-4 backlog to CARRY FORWARD (record, do not silently drop): egress `on_frame` blocking `send` on the QUIC thread and gateway reconnect are gateway-internal, out of scope here; TSAN CI job; known QUIC integration test #63 flake; ack seq-number 32-bit wrap; Reader internal bounds asserts + fuzzing; `outbound_queue` unbounded/stale-drop; Listener session reaping. These are tracked in `.superpowers/sdd/progress.md`; the final review triages them.
- Integration tests: loopback, fixed ports 45600-45619, bounded waits. Reuse the cert fixture (`ROQR_TEST_CERT_DIR`).
- Commit messages: plain imperative, no emoji, no Claude tagline, no Co-Authored-By. TDD per task.
- Build/test: `cmake --build --preset dev && ctest --preset dev` (reconfigure needs `eval "$(scripts/setup_picoquic_deps.sh)"` first). Baseline at plan start: 136 tests green. The JDK for the desktop JNI build is at `/usr/lib/jvm/jdk-21-oracle-x64` (found via `find_package(JNI)`; do not hardcode the path).

---

### Task 1: FFI skeleton — roqr.h C ABI, client create/destroy, version

**Files:**
- Create: `ffi/CMakeLists.txt`
- Create: `ffi/include/roqr/roqr.h`
- Create: `ffi/src/roqr_ffi.cpp`
- Modify: `CMakeLists.txt` (root: `ROQR_BUILD_FFI` option + subdir)
- Modify: `tests/CMakeLists.txt` (new `roqr-ffi-tests` target, C++ Catch2 including the C header under `extern "C"`)
- Test: `tests/ffi/ffi_smoke_test.cpp`

**Interfaces:**
- Consumes: `roqr::quic::Client` (quic).
- Produces (C ABI, in `roqr.h`):
  - `typedef struct roqr_client roqr_client;` (opaque).
  - `typedef enum roqr_error { ROQR_OK = 0, ROQR_ERR_GENERAL = 1, ROQR_ERR_INTERNAL = 2, ROQR_ERR_FRAME_ENCODING = 3, ROQR_ERR_STREAM_CREATION = 4, ROQR_ERR_FRAME_CANCELLED = 5, ROQR_ERR_UNKNOWN_FLOW = 6, ROQR_ERR_EXPECTATION_UNMET = 7, ROQR_ERR_INVALID_ARG = 100, ROQR_ERR_CONNECT_FAILED = 101, ROQR_ERR_TIMEOUT = 102 } roqr_error;` (0x00-0x07 mirror draft Table 2; 100+ are FFI-local).
  - `typedef enum roqr_delivery_mode { ROQR_DELIVERY_STREAM = 0, ROQR_DELIVERY_DATAGRAM = 1, ROQR_DELIVERY_AUTO = 2 } roqr_delivery_mode;`
  - `typedef struct roqr_frame { uint64_t flow_id; uint64_t timestamp; uint8_t message_type; uint64_t message_stream_id; uint64_t chunk_stream_id; const uint8_t* payload; size_t payload_len; } roqr_frame;`
  - `const char* roqr_version(void);`
  - `roqr_client* roqr_client_create(void);` / `void roqr_client_destroy(roqr_client*);`

- [ ] **Step 1: Create `ffi/include/roqr/roqr.h`**

```c
#ifndef ROQR_H
#define ROQR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes. 0x00-0x07 mirror the RoQR draft Table 2 application error
 * codes; values >= 100 are FFI-local. */
typedef enum roqr_error {
    ROQR_OK = 0,
    ROQR_ERR_GENERAL = 1,
    ROQR_ERR_INTERNAL = 2,
    ROQR_ERR_FRAME_ENCODING = 3,
    ROQR_ERR_STREAM_CREATION = 4,
    ROQR_ERR_FRAME_CANCELLED = 5,
    ROQR_ERR_UNKNOWN_FLOW = 6,
    ROQR_ERR_EXPECTATION_UNMET = 7,
    ROQR_ERR_INVALID_ARG = 100,
    ROQR_ERR_CONNECT_FAILED = 101,
    ROQR_ERR_TIMEOUT = 102
} roqr_error;

typedef enum roqr_delivery_mode {
    ROQR_DELIVERY_STREAM = 0,
    ROQR_DELIVERY_DATAGRAM = 1,
    ROQR_DELIVERY_AUTO = 2
} roqr_delivery_mode;

/* One RoQR frame crossing the ABI. On the receive callback, `payload`
 * points into memory owned by the library and valid only for the duration
 * of the callback; copy it if you need it later. */
typedef struct roqr_frame {
    uint64_t flow_id;
    uint64_t timestamp;
    uint8_t message_type;
    uint64_t message_stream_id;
    uint64_t chunk_stream_id;
    const uint8_t* payload;
    size_t payload_len;
} roqr_frame;

typedef struct roqr_client roqr_client;

/* Library version, "major.minor.patch". */
const char* roqr_version(void);

roqr_client* roqr_client_create(void);
void roqr_client_destroy(roqr_client* client);

#ifdef __cplusplus
}
#endif

#endif /* ROQR_H */
```

- [ ] **Step 2: Write the failing test `tests/ffi/ffi_smoke_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include <cstring>

extern "C" {
#include "roqr/roqr.h"
}

TEST_CASE("ffi version matches the library version") {
    CHECK(std::strcmp(roqr_version(), "0.1.0") == 0);
}

TEST_CASE("ffi client create and destroy") {
    roqr_client* c = roqr_client_create();
    REQUIRE(c != nullptr);
    roqr_client_destroy(c);
    roqr_client_destroy(nullptr);  // must be a safe no-op
}

TEST_CASE("ffi error and delivery enum values are stable") {
    CHECK(ROQR_OK == 0);
    CHECK(ROQR_ERR_UNKNOWN_FLOW == 6);
    CHECK(ROQR_DELIVERY_AUTO == 2);
}
```

- [ ] **Step 3: Wire the build**

`ffi/CMakeLists.txt`:

```cmake
add_library(roqr-ffi SHARED
  src/roqr_ffi.cpp
)

target_include_directories(roqr-ffi PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(roqr-ffi PRIVATE roqr-core roqr-quic roqr-gateway)

target_compile_features(roqr-ffi PRIVATE cxx_std_20)
target_compile_options(roqr-ffi PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
```

Root `CMakeLists.txt`: add below the existing options:

```cmake
option(ROQR_BUILD_FFI "Build the C FFI shared library" ON)
```

and after the examples/gateway block (FFI wraps the Client + the roqr-gateway
lib, which builds under QUIC+RTMP; skip silently when they are off so the
core-only CI job — which sets QUIC=OFF — does not break):

```cmake
if(ROQR_BUILD_FFI)
  if(ROQR_BUILD_QUIC AND ROQR_BUILD_RTMP)
    add_subdirectory(ffi)
  else()
    message(STATUS "ROQR_BUILD_FFI needs QUIC and RTMP; skipping FFI in this config")
  endif()
endif()
```

`tests/CMakeLists.txt`: append after the gateway/e2e block (gate on the
target actually existing, so a config that skipped FFI does not reference it):

```cmake
if(ROQR_BUILD_FFI AND TARGET roqr-ffi)
  add_executable(roqr-ffi-tests
    ffi/ffi_smoke_test.cpp
  )
  target_link_libraries(roqr-ffi-tests PRIVATE roqr-ffi Catch2::Catch2WithMain)
  catch_discover_tests(roqr-ffi-tests PROPERTIES TIMEOUT 60)
endif()
```

- [ ] **Step 4: Run to verify RED**

Run: `cmake --preset dev && cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to link (missing `roqr_version`/`roqr_client_create`/`roqr_client_destroy`).

- [ ] **Step 5: Implement `ffi/src/roqr_ffi.cpp`**

```cpp
#include "roqr/roqr.h"

#include <memory>

#include "roqr/quic/client.hpp"

// The opaque roqr_client owns a roqr::quic::Client plus the C callback
// pointers registered against it (added in Task 2).
struct roqr_client {
    roqr::quic::Client client;
};

extern "C" {

const char* roqr_version(void) { return "0.1.0"; }

roqr_client* roqr_client_create(void) {
    return new (std::nothrow) roqr_client();
}

void roqr_client_destroy(roqr_client* client) { delete client; }

}  // extern "C"
```

- [ ] **Step 6: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 136 prior + 3 new = 139.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt ffi tests
git commit -m "Add C FFI skeleton with client handle and version"
```

---

### Task 2: FFI client connect, send, callbacks, close

**Files:**
- Modify: `ffi/include/roqr/roqr.h` (client lifecycle + callback functions)
- Modify: `ffi/src/roqr_ffi.cpp`
- Modify: `tests/CMakeLists.txt` (integration test needs the relay + cert fixture)
- Test: `tests/integration/ffi_client_test.cpp`

**Interfaces:**
- Consumes: Task 1's `roqr_client`, `roqr_frame`, `roqr_error`, `roqr_delivery_mode`; `roqr::quic::Client` methods; `roqr::relayd::Server` (test only).
- Produces (C ABI additions to `roqr.h`):
  - `typedef void (*roqr_message_cb)(const roqr_frame* frame, void* user_data);`
  - `typedef void (*roqr_closed_cb)(uint64_t app_error_code, void* user_data);`
  - `void roqr_client_set_on_message(roqr_client*, roqr_message_cb, void* user_data);` (set before connect; fires on the network thread)
  - `void roqr_client_set_on_closed(roqr_client*, roqr_closed_cb, void* user_data);`
  - `roqr_error roqr_client_connect(roqr_client*, const char* host, uint16_t port, int insecure_skip_verify);`
  - `int roqr_client_wait_connected(roqr_client*, int timeout_ms);` (1 = connected, 0 = timeout)
  - `int roqr_client_datagrams_negotiated(roqr_client*);`
  - `roqr_error roqr_client_send(roqr_client*, const roqr_frame* frame, roqr_delivery_mode mode);` (thread-safe; ROQR_ERR_INVALID_ARG on empty payload)
  - `void roqr_client_bind_flow(roqr_client*, uint64_t flow_id);` / `void roqr_client_retire_flow(roqr_client*, uint64_t flow_id);`
  - `void roqr_client_close(roqr_client*, uint64_t app_error_code);`
  - `int roqr_client_wait_closed(roqr_client*, int timeout_ms);`

- [ ] **Step 1: Write the failing test `tests/integration/ffi_client_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "roqr/relayd/server.hpp"

extern "C" {
#include "roqr/roqr.h"
}

using namespace std::chrono_literals;

namespace {
const std::string kCertDir = ROQR_TEST_CERT_DIR;

struct Sink {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::vector<uint8_t>> payloads;
    std::atomic<bool> closed{false};
};

void on_message(const roqr_frame* f, void* user) {
    auto* s = static_cast<Sink*>(user);
    std::lock_guard lock(s->mutex);
    s->payloads.emplace_back(f->payload, f->payload + f->payload_len);
    s->cv.notify_all();
}

void on_closed(uint64_t /*code*/, void* user) {
    static_cast<Sink*>(user)->closed = true;
}

roqr_frame make_frame(uint64_t ts, const std::vector<uint8_t>& payload) {
    roqr_frame f{};
    f.flow_id = 0;
    f.timestamp = ts;
    f.message_type = 9;
    f.message_stream_id = 1;
    f.chunk_stream_id = 6;
    f.payload = payload.data();
    f.payload_len = payload.size();
    return f;
}
}  // namespace

TEST_CASE("ffi client echoes a frame through the relay") {
    roqr::relayd::Server server;
    roqr::relayd::ServerOptions so;
    so.port = 45600;
    so.cert_file = kCertDir + "/cert.pem";
    so.key_file = kCertDir + "/key.pem";
    so.mode = roqr::relayd::Mode::Echo;
    REQUIRE(server.start(so));

    Sink sink;
    roqr_client* c = roqr_client_create();
    roqr_client_set_on_message(c, on_message, &sink);
    roqr_client_set_on_closed(c, on_closed, &sink);
    REQUIRE(roqr_client_connect(c, "127.0.0.1", 45600, 1) == ROQR_OK);
    REQUIRE(roqr_client_wait_connected(c, 5000) == 1);

    const std::vector<uint8_t> payload = {0x17, 0x01, 0xAB};
    const roqr_frame f = make_frame(100, payload);
    REQUIRE(roqr_client_send(c, &f, ROQR_DELIVERY_STREAM) == ROQR_OK);

    {
        std::unique_lock lock(sink.mutex);
        REQUIRE(sink.cv.wait_for(lock, 5s,
                                 [&] { return !sink.payloads.empty(); }));
        CHECK(sink.payloads[0] == payload);
    }

    roqr_client_close(c, 0);
    CHECK(roqr_client_wait_closed(c, 5000) == 1);
    roqr_client_destroy(c);
    server.stop();
}

TEST_CASE("ffi send rejects an empty payload and validates args") {
    roqr_client* c = roqr_client_create();
    const roqr_frame empty = make_frame(1, {});
    CHECK(roqr_client_send(c, &empty, ROQR_DELIVERY_STREAM) ==
          ROQR_ERR_INVALID_ARG);
    CHECK(roqr_client_send(nullptr, &empty, ROQR_DELIVERY_STREAM) ==
          ROQR_ERR_INVALID_ARG);
    roqr_client_destroy(c);
}

TEST_CASE("ffi wait_connected times out without a server") {
    roqr_client* c = roqr_client_create();
    REQUIRE(roqr_client_connect(c, "127.0.0.1", 45601, 1) == ROQR_OK);
    CHECK(roqr_client_wait_connected(c, 1500) == 0);
    roqr_client_destroy(c);
}
```

Add `integration/ffi_client_test.cpp` to `roqr-integration-tests` and link `roqr-ffi` into it.

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to compile (missing `roqr_client_connect` etc.).

- [ ] **Step 3: Add the declarations to `ffi/include/roqr/roqr.h`** (before the closing `#ifdef __cplusplus`):

```c
/* Receive callback: fires on the QUIC network thread. Do NOT block and do
 * NOT call roqr_client_destroy or roqr_client_wait_* from inside it;
 * roqr_client_send/close/bind_flow/retire_flow are safe to call. `frame`
 * and its payload are valid only for the duration of the call. */
typedef void (*roqr_message_cb)(const roqr_frame* frame, void* user_data);

/* Close callback: fires on the network thread with the peer's application
 * error code (0 for a clean close). Same non-blocking rules apply. */
typedef void (*roqr_closed_cb)(uint64_t app_error_code, void* user_data);

/* Set handlers before roqr_client_connect. */
void roqr_client_set_on_message(roqr_client* client, roqr_message_cb cb,
                                void* user_data);
void roqr_client_set_on_closed(roqr_client* client, roqr_closed_cb cb,
                               void* user_data);

roqr_error roqr_client_connect(roqr_client* client, const char* host,
                               uint16_t port, int insecure_skip_verify);
/* Returns 1 if connected within timeout_ms, 0 on timeout. */
int roqr_client_wait_connected(roqr_client* client, int timeout_ms);
int roqr_client_datagrams_negotiated(roqr_client* client);

/* Thread-safe. Returns ROQR_ERR_INVALID_ARG for a null client/frame or an
 * empty payload (RoQR requires payload length > 0). */
roqr_error roqr_client_send(roqr_client* client, const roqr_frame* frame,
                            roqr_delivery_mode mode);

void roqr_client_bind_flow(roqr_client* client, uint64_t flow_id);
void roqr_client_retire_flow(roqr_client* client, uint64_t flow_id);

void roqr_client_close(roqr_client* client, uint64_t app_error_code);
int roqr_client_wait_closed(roqr_client* client, int timeout_ms);
```

- [ ] **Step 4: Implement in `ffi/src/roqr_ffi.cpp`**

Extend the `roqr_client` struct and add the functions:

```cpp
#include <chrono>
#include <cstring>

#include "roqr/error.hpp"

struct roqr_client {
    roqr::quic::Client client;
    roqr_message_cb on_message = nullptr;
    void* on_message_user = nullptr;
    roqr_closed_cb on_closed = nullptr;
    void* on_closed_user = nullptr;
};

namespace {

roqr::quic::DeliveryMode to_cpp_mode(roqr_delivery_mode m) {
    switch (m) {
        case ROQR_DELIVERY_DATAGRAM:
            return roqr::quic::DeliveryMode::Datagram;
        case ROQR_DELIVERY_AUTO:
            return roqr::quic::DeliveryMode::Auto;
        case ROQR_DELIVERY_STREAM:
        default:
            return roqr::quic::DeliveryMode::Stream;
    }
}

}  // namespace

extern "C" {

void roqr_client_set_on_message(roqr_client* c, roqr_message_cb cb,
                                void* user) {
    if (c == nullptr) return;
    c->on_message = cb;
    c->on_message_user = user;
}

void roqr_client_set_on_closed(roqr_client* c, roqr_closed_cb cb,
                               void* user) {
    if (c == nullptr) return;
    c->on_closed = cb;
    c->on_closed_user = user;
}

roqr_error roqr_client_connect(roqr_client* c, const char* host,
                               uint16_t port, int insecure_skip_verify) {
    if (c == nullptr || host == nullptr) return ROQR_ERR_INVALID_ARG;

    c->client.on_message([c](const roqr::Frame& f) {
        if (c->on_message == nullptr) return;
        roqr_frame cf{};
        cf.flow_id = f.flow_id;
        cf.timestamp = f.timestamp;
        cf.message_type = f.message_type;
        cf.message_stream_id = f.message_stream_id;
        cf.chunk_stream_id = f.chunk_stream_id;
        cf.payload = f.payload.data();
        cf.payload_len = f.payload.size();
        c->on_message(&cf, c->on_message_user);
    });
    c->client.on_closed([c](uint64_t code) {
        if (c->on_closed != nullptr) c->on_closed(code, c->on_closed_user);
    });

    roqr::quic::ClientOptions opts;
    opts.insecure_skip_verify = insecure_skip_verify != 0;
    if (!c->client.connect(host, port, opts)) return ROQR_ERR_CONNECT_FAILED;
    return ROQR_OK;
}

int roqr_client_wait_connected(roqr_client* c, int timeout_ms) {
    if (c == nullptr) return 0;
    return c->client.wait_connected(std::chrono::milliseconds(timeout_ms))
               ? 1
               : 0;
}

int roqr_client_datagrams_negotiated(roqr_client* c) {
    return (c != nullptr && c->client.datagrams_negotiated()) ? 1 : 0;
}

roqr_error roqr_client_send(roqr_client* c, const roqr_frame* frame,
                            roqr_delivery_mode mode) {
    if (c == nullptr || frame == nullptr || frame->payload_len == 0) {
        return ROQR_ERR_INVALID_ARG;
    }
    roqr::Frame f;
    f.flow_id = frame->flow_id;
    f.timestamp = frame->timestamp;
    f.message_type = frame->message_type;
    f.message_stream_id = frame->message_stream_id;
    f.chunk_stream_id = frame->chunk_stream_id;
    f.payload.assign(frame->payload, frame->payload + frame->payload_len);
    return c->client.send(std::move(f), to_cpp_mode(mode)) ? ROQR_OK
                                                           : ROQR_ERR_GENERAL;
}

void roqr_client_bind_flow(roqr_client* c, uint64_t flow_id) {
    if (c != nullptr) c->client.bind_flow(flow_id);
}

void roqr_client_retire_flow(roqr_client* c, uint64_t flow_id) {
    if (c != nullptr) c->client.retire_flow(flow_id);
}

void roqr_client_close(roqr_client* c, uint64_t app_error_code) {
    if (c != nullptr) {
        c->client.close(static_cast<roqr::ErrorCode>(app_error_code));
    }
}

int roqr_client_wait_closed(roqr_client* c, int timeout_ms) {
    if (c == nullptr) return 0;
    return c->client.wait_closed(std::chrono::milliseconds(timeout_ms)) ? 1
                                                                        : 0;
}

}  // extern "C"
```

(The `roqr_client_create`/`destroy`/`roqr_version` from Task 1 stay; move the `struct roqr_client` definition to the top so all functions see the extended layout.)

- [ ] **Step 5: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 139 prior + 3 new = 142. `--repeat until-fail:2` on the ffi_client integration test for stability.

- [ ] **Step 6: Commit**

```bash
git add ffi tests
git commit -m "Add FFI client connect, send, callbacks, and close"
```

---

### Task 3: FFI gateways — roqr_rtmp.h ingest and egress

**Files:**
- Create: `ffi/include/roqr/roqr_rtmp.h`
- Create: `ffi/src/roqr_rtmp_ffi.cpp`
- Modify: `ffi/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/integration/ffi_gateway_test.cpp`

**Interfaces:**
- Consumes: `roqr_error` (roqr.h); `roqr::gateway::IngestGateway`/`IngestOptions`, `EgressGateway`/`EgressOptions`.
- Produces (C ABI, in `roqr_rtmp.h`):
  - `typedef struct roqr_ingest roqr_ingest;` / `typedef struct roqr_egress roqr_egress;`
  - `roqr_ingest* roqr_ingest_create(void);` / `void roqr_ingest_destroy(roqr_ingest*);`
  - `roqr_error roqr_ingest_start(roqr_ingest*, uint16_t rtmp_port, const char* roqr_host, uint16_t roqr_port, int insecure_skip_verify);` (ROQR_OK = RTMP listener up; NOT that the RoQR leg is live — wait for readiness)
  - `int roqr_ingest_wait_publishing(roqr_ingest*, int timeout_ms);`
  - `void roqr_ingest_stop(roqr_ingest*);`
  - `roqr_egress* roqr_egress_create(void);` / `void roqr_egress_destroy(roqr_egress*);`
  - `roqr_error roqr_egress_start(roqr_egress*, uint16_t rtmp_port, const char* roqr_host, uint16_t roqr_port, const char* stream_name, int insecure_skip_verify);`
  - `int roqr_egress_wait_playing(roqr_egress*, int timeout_ms);`
  - `void roqr_egress_stop(roqr_egress*);`

- [ ] **Step 1: Write the failing test `tests/integration/ffi_gateway_test.cpp`**

The test stands up a `Mode::Media` relay, an FFI ingest and FFI egress, publishes via the FFI client sending RTMP-command + media frames through ingest's... simpler: reuse the C++ minimal RTMP publisher/player from the Plan 4 gateway tests is heavy. Instead assert the FFI gateway lifecycle drives the C++ gateway: an FFI ingest in front of a media relay, plus an FFI egress playing, with a raw RTMP publisher (HandshakeInitiator+ChunkWriter) into ingest and a raw RTMP player out of egress — verify the player receives video. To keep the test file self-contained, it uses the same minimal RTMP client structs as `tests/integration/ingest_test.cpp` / `egress_test.cpp`.

```cpp
#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "roqr/gateway/rtmp_commands.hpp"
#include "roqr/relayd/server.hpp"
#include "roqr/rtmp/chunk_reader.hpp"
#include "roqr/rtmp/chunk_writer.hpp"
#include "roqr/rtmp/handshake.hpp"

extern "C" {
#include "roqr/roqr_rtmp.h"
}

using namespace std::chrono_literals;

namespace {
const std::string kCertDir = ROQR_TEST_CERT_DIR;

bool send_all(int fd, const std::vector<uint8_t>& d) {
    size_t off = 0;
    while (off < d.size()) {
        ssize_t n = ::send(fd, d.data() + off, d.size() - off, 0);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

int connect_tcp(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool do_handshake_client(int fd, roqr::rtmp::HandshakeInitiator& hs) {
    if (!send_all(fd, hs.start())) return false;
    uint8_t buf[4096];
    std::vector<uint8_t> c2;
    while (!hs.done()) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        if (!hs.feed(std::span<const uint8_t>(buf, static_cast<size_t>(n)),
                     c2)) {
            return false;
        }
        if (!c2.empty()) {
            send_all(fd, c2);
            c2.clear();
        }
    }
    return true;
}
}  // namespace

TEST_CASE("ffi ingest and egress carry video end to end") {
    roqr::relayd::Server relay;
    roqr::relayd::ServerOptions ro;
    ro.port = 45602;
    ro.cert_file = kCertDir + "/cert.pem";
    ro.key_file = kCertDir + "/key.pem";
    ro.mode = roqr::relayd::Mode::Media;
    REQUIRE(relay.start(ro));

    roqr_egress* eg = roqr_egress_create();
    REQUIRE(roqr_egress_start(eg, 45604, "127.0.0.1", 45602, "cam", 1) ==
            ROQR_OK);
    REQUIRE(roqr_egress_wait_playing(eg, 5000) == 1);

    roqr_ingest* in = roqr_ingest_create();
    REQUIRE(roqr_ingest_start(in, 45603, "127.0.0.1", 45602, 1) == ROQR_OK);

    // RTMP publisher into ingest.
    roqr::rtmp::HandshakeInitiator phs;
    roqr::rtmp::ChunkWriter pw;
    int pub = connect_tcp(45603);
    REQUIRE(pub >= 0);
    REQUIRE(do_handshake_client(pub, phs));
    auto send_cmd = [&](const roqr::rtmp::RtmpMessage& m) {
        std::vector<uint8_t> wire;
        pw.write(m, wire);
        send_all(pub, wire);
    };
    send_cmd(roqr::gateway::build_connect(1, "live", "rtmp://h/live"));
    send_cmd(roqr::gateway::build_create_stream(2));
    send_cmd(roqr::gateway::build_publish(3, "cam"));
    REQUIRE(roqr_ingest_wait_publishing(in, 5000) == 1);

    // RTMP player out of egress.
    roqr::rtmp::HandshakeInitiator lhs;
    roqr::rtmp::ChunkWriter lw;
    roqr::rtmp::ChunkReader lr;
    int play = connect_tcp(45604);
    REQUIRE(play >= 0);
    REQUIRE(do_handshake_client(play, lhs));
    auto play_cmd = [&](const roqr::rtmp::RtmpMessage& m) {
        std::vector<uint8_t> wire;
        lw.write(m, wire);
        send_all(play, wire);
    };
    play_cmd(roqr::gateway::build_connect(1, "live", "rtmp://h/live"));
    play_cmd(roqr::gateway::build_create_stream(2));
    play_cmd(roqr::gateway::build_play(3, "cam"));
    std::this_thread::sleep_for(200ms);

    // Publish a seq header + keyframe.
    auto vid = [&](uint32_t ts, std::vector<uint8_t> p) {
        roqr::rtmp::RtmpMessage m;
        m.type = 9;
        m.timestamp = ts;
        m.message_stream_id = 1;
        m.chunk_stream_id = 6;
        m.payload = std::move(p);
        std::vector<uint8_t> wire;
        pw.write(m, wire);
        send_all(pub, wire);
    };
    vid(0, {0x17, 0x00, 0x11});
    vid(40, {0x17, 0x01, 0x22});

    // The player must receive a video message.
    bool got_video = false;
    uint8_t buf[4096];
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!got_video && std::chrono::steady_clock::now() < deadline) {
        while (auto m = lr.next()) {
            if (m->type == 9) got_video = true;
        }
        if (got_video) break;
        ssize_t n = ::recv(play, buf, sizeof(buf), 0);
        if (n <= 0) break;
        lr.feed(std::span<const uint8_t>(buf, static_cast<size_t>(n)));
    }
    CHECK(got_video);

    ::close(pub);
    ::close(play);
    roqr_ingest_stop(in);
    roqr_ingest_destroy(in);
    roqr_egress_stop(eg);
    roqr_egress_destroy(eg);
    relay.stop();
}

TEST_CASE("ffi gateway create/destroy and null-arg safety") {
    roqr_ingest* in = roqr_ingest_create();
    REQUIRE(in != nullptr);
    roqr_ingest_stop(in);        // stop before start is safe
    roqr_ingest_destroy(in);
    roqr_ingest_destroy(nullptr);

    roqr_egress* eg = roqr_egress_create();
    REQUIRE(eg != nullptr);
    CHECK(roqr_egress_start(nullptr, 1, "h", 2, "s", 1) ==
          ROQR_ERR_INVALID_ARG);
    roqr_egress_destroy(eg);
}
```

Add `integration/ffi_gateway_test.cpp` to `roqr-integration-tests`.

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to compile (missing `roqr/roqr_rtmp.h`).

- [ ] **Step 3: Create `ffi/include/roqr/roqr_rtmp.h`**

```c
#ifndef ROQR_RTMP_H
#define ROQR_RTMP_H

#include <stdint.h>

#include "roqr/roqr.h"  /* roqr_error */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct roqr_ingest roqr_ingest;
typedef struct roqr_egress roqr_egress;

roqr_ingest* roqr_ingest_create(void);
void roqr_ingest_destroy(roqr_ingest* ingest);
/* Starts the RTMP listener on rtmp_port and connects to the RoQR server on
 * publish. ROQR_OK means the RTMP listener is up; it does NOT mean the RoQR
 * server leg is live. Wait for roqr_ingest_wait_publishing to confirm the
 * end-to-end path. */
roqr_error roqr_ingest_start(roqr_ingest* ingest, uint16_t rtmp_port,
                             const char* roqr_host, uint16_t roqr_port,
                             int insecure_skip_verify);
int roqr_ingest_wait_publishing(roqr_ingest* ingest, int timeout_ms);
void roqr_ingest_stop(roqr_ingest* ingest);

roqr_egress* roqr_egress_create(void);
void roqr_egress_destroy(roqr_egress* egress);
/* Starts the RTMP listener on rtmp_port and plays stream_name from the RoQR
 * server. Same readiness caveat as ingest: wait for
 * roqr_egress_wait_playing. */
roqr_error roqr_egress_start(roqr_egress* egress, uint16_t rtmp_port,
                             const char* roqr_host, uint16_t roqr_port,
                             const char* stream_name,
                             int insecure_skip_verify);
int roqr_egress_wait_playing(roqr_egress* egress, int timeout_ms);
void roqr_egress_stop(roqr_egress* egress);

#ifdef __cplusplus
}
#endif

#endif /* ROQR_RTMP_H */
```

- [ ] **Step 4: Implement `ffi/src/roqr_rtmp_ffi.cpp`**

```cpp
#include "roqr/roqr_rtmp.h"

#include <chrono>
#include <string>

#include "roqr/gateway/egress.hpp"
#include "roqr/gateway/ingest.hpp"

struct roqr_ingest {
    roqr::gateway::IngestGateway gateway;
};
struct roqr_egress {
    roqr::gateway::EgressGateway gateway;
};

extern "C" {

roqr_ingest* roqr_ingest_create(void) {
    return new (std::nothrow) roqr_ingest();
}
void roqr_ingest_destroy(roqr_ingest* in) { delete in; }

roqr_error roqr_ingest_start(roqr_ingest* in, uint16_t rtmp_port,
                             const char* roqr_host, uint16_t roqr_port,
                             int insecure_skip_verify) {
    if (in == nullptr || roqr_host == nullptr) return ROQR_ERR_INVALID_ARG;
    roqr::gateway::IngestOptions o;
    o.rtmp_port = rtmp_port;
    o.roqr_host = roqr_host;
    o.roqr_port = roqr_port;
    o.insecure_skip_verify = insecure_skip_verify != 0;
    return in->gateway.start(o) ? ROQR_OK : ROQR_ERR_GENERAL;
}

int roqr_ingest_wait_publishing(roqr_ingest* in, int timeout_ms) {
    if (in == nullptr) return 0;
    return in->gateway.wait_publishing(std::chrono::milliseconds(timeout_ms))
               ? 1
               : 0;
}

void roqr_ingest_stop(roqr_ingest* in) {
    if (in != nullptr) in->gateway.stop();
}

roqr_egress* roqr_egress_create(void) {
    return new (std::nothrow) roqr_egress();
}
void roqr_egress_destroy(roqr_egress* eg) { delete eg; }

roqr_error roqr_egress_start(roqr_egress* eg, uint16_t rtmp_port,
                             const char* roqr_host, uint16_t roqr_port,
                             const char* stream_name,
                             int insecure_skip_verify) {
    if (eg == nullptr || roqr_host == nullptr || stream_name == nullptr) {
        return ROQR_ERR_INVALID_ARG;
    }
    roqr::gateway::EgressOptions o;
    o.rtmp_port = rtmp_port;
    o.roqr_host = roqr_host;
    o.roqr_port = roqr_port;
    o.stream_name = stream_name;
    o.insecure_skip_verify = insecure_skip_verify != 0;
    return eg->gateway.start(o) ? ROQR_OK : ROQR_ERR_GENERAL;
}

int roqr_egress_wait_playing(roqr_egress* eg, int timeout_ms) {
    if (eg == nullptr) return 0;
    return eg->gateway.wait_playing(std::chrono::milliseconds(timeout_ms))
               ? 1
               : 0;
}

void roqr_egress_stop(roqr_egress* eg) {
    if (eg != nullptr) eg->gateway.stop();
}

}  // extern "C"
```

Add `src/roqr_rtmp_ffi.cpp` to `ffi/CMakeLists.txt` and link `roqr-gateway` (already linked).

- [ ] **Step 5: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 142 prior + 2 new = 144. `--repeat until-fail:2` on the ffi_gateway test.

- [ ] **Step 6: Commit**

```bash
git add ffi tests
git commit -m "Add FFI ingest and egress gateway wrappers"
```

---
### Task 4: JNI build skeleton — org.red5.roqr, RoqrNative, Java smoke test

**Files:**
- Create: `jni/CMakeLists.txt`
- Create: `jni/src/roqr_jni.cpp`
- Create: `jni/java/org/red5/roqr/RoqrNative.java`
- Create: `jni/java/org/red5/roqr/package-info.java`
- Create: `tests/jni/RoqrSmokeTest.java`
- Create: `tests/jni/CMakeLists.txt`
- Modify: `CMakeLists.txt` (root: `ROQR_BUILD_JNI` option + subdir), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: the C FFI (`roqr.h`) — the JNI lib links `roqr-ffi`.
- Produces:
  - CMake option `ROQR_BUILD_JNI` (OFF default; `find_package(JNI)`, skip cleanly if not found); target `roqr-jni` (SHARED, links roqr-ffi + JNI::JNI); a compiled `roqr.jar` via `UseJava`.
  - `org.red5.roqr.RoqrNative` with `public static native String version();` and `static { System.loadLibrary("roqr-jni"); }`.
  - `JavaVM*` cached in `JNI_OnLoad` (returns `JNI_VERSION_1_6`) for later thread attachment.
  - A CTest test `roqr-jni-smoke` that runs `java -Djava.library.path=<libdir> -cp <jar>:<testdir> RoqrSmokeTest` asserting `RoqrNative.version()` equals the library version.

- [ ] **Step 1: Create the Java class `jni/java/org/red5/roqr/RoqrNative.java`**

```java
package org.red5.roqr;

/** Low-level JNI entry points. Loads the native roqr-jni library. */
public final class RoqrNative {
    static {
        System.loadLibrary("roqr-jni");
    }

    private RoqrNative() {}

    /** Native library version, "major.minor.patch". */
    public static native String version();
}
```

`jni/java/org/red5/roqr/package-info.java`:

```java
/** RTMP over QUIC (RoQR) Java bindings. */
package org.red5.roqr;
```

- [ ] **Step 2: Write the failing test `tests/jni/RoqrSmokeTest.java`**

```java
import org.red5.roqr.RoqrNative;

public class RoqrSmokeTest {
    public static void main(String[] args) {
        String v = RoqrNative.version();
        if (!"0.1.0".equals(v)) {
            System.err.println("FAIL: version=" + v + " expected 0.1.0");
            System.exit(1);
        }
        System.out.println("PASS: RoqrNative.version()=" + v);
    }
}
```

- [ ] **Step 3: Wire the build**

Root `CMakeLists.txt`: add below the FFI option:

```cmake
option(ROQR_BUILD_JNI "Build JNI bindings (needs a JDK)" OFF)
```

and after the FFI subdir block:

```cmake
if(ROQR_BUILD_JNI)
  if(NOT TARGET roqr-ffi)
    message(FATAL_ERROR "ROQR_BUILD_JNI requires the FFI library (ROQR_BUILD_FFI plus QUIC+RTMP)")
  endif()
  find_package(JNI)
  if(JNI_FOUND)
    add_subdirectory(jni)
  else()
    message(WARNING "JNI not found; skipping roqr-jni")
  endif()
endif()
```

**Placement note:** this JNI block must come AFTER the `if(ROQR_BUILD_FFI) ... add_subdirectory(ffi) endif()` block (so the `roqr-ffi` target exists to test for).

`jni/CMakeLists.txt`:

```cmake
find_package(Java REQUIRED COMPONENTS Development)
include(UseJava)

add_library(roqr-jni SHARED
  src/roqr_jni.cpp
)
target_include_directories(roqr-jni PRIVATE ${JNI_INCLUDE_DIRS})
target_link_libraries(roqr-jni PRIVATE roqr-ffi ${JNI_LIBRARIES})
target_compile_features(roqr-jni PRIVATE cxx_std_20)
target_compile_options(roqr-jni PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)

add_jar(roqr-jar
  SOURCES
    java/org/red5/roqr/RoqrNative.java
    java/org/red5/roqr/package-info.java
  OUTPUT_NAME roqr)

# Expose the jar path and the jni lib dir to the test subdirectory.
get_target_property(ROQR_JAR_FILE roqr-jar JAR_FILE)
set(ROQR_JAR_FILE "${ROQR_JAR_FILE}" CACHE INTERNAL "roqr jar path")
```

Add `ROQR_BUILD_JNI` to the dev preset in `CMakePresets.json` (`"ROQR_BUILD_JNI": "ON"`).

Enable the dev preset to actually build JNI: append the test subdir in `tests/CMakeLists.txt`:

```cmake
if(ROQR_BUILD_JNI AND TARGET roqr-jni)
  add_subdirectory(jni)
endif()
```

`tests/jni/CMakeLists.txt`:

```cmake
find_package(Java REQUIRED COMPONENTS Runtime)

# Compile the Java test against the roqr jar.
add_jar(roqr-jni-smoke-test
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/RoqrSmokeTest.java
  INCLUDE_JARS roqr-jar)
get_target_property(ROQR_SMOKE_JAR roqr-jni-smoke-test JAR_FILE)

add_test(
  NAME roqr-jni-smoke
  COMMAND ${Java_JAVA_EXECUTABLE}
    -Djava.library.path=$<TARGET_FILE_DIR:roqr-jni>
    -cp "${ROQR_JAR_FILE}:${ROQR_SMOKE_JAR}"
    RoqrSmokeTest)
set_tests_properties(roqr-jni-smoke PROPERTIES TIMEOUT 60)
```

- [ ] **Step 4: Run to verify RED**

Run: `cmake --preset dev && cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to link (missing `Java_org_red5_roqr_RoqrNative_version`). If `find_package(JNI)` fails on this machine, that is a setup problem — the JDK is at `/usr/lib/jvm/jdk-21-oracle-x64`; ensure `JAVA_HOME` is set or CMake finds it. Do not proceed until JNI is found.

- [ ] **Step 5: Implement `jni/src/roqr_jni.cpp`**

```cpp
#include <jni.h>

extern "C" {
#include "roqr/roqr.h"
}

namespace {
JavaVM* g_vm = nullptr;
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_red5_roqr_RoqrNative_version(JNIEnv* env, jclass /*clazz*/) {
    return env->NewStringUTF(roqr_version());
}
```

- [ ] **Step 6: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev -R roqr-jni-smoke --output-on-failure`
Expected: PASS — the Java smoke test prints `PASS: RoqrNative.version()=0.1.0`. Full suite `ctest --preset dev`: 144 prior + 1 jni-smoke = 145.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt CMakePresets.json jni tests
git commit -m "Add JNI build skeleton with RoqrNative version smoke test"
```

---

### Task 5: JNI client — RoqrClient, Frame, DeliveryMode, native-thread callbacks

**Files:**
- Create: `jni/java/org/red5/roqr/RoqrClient.java`
- Create: `jni/java/org/red5/roqr/Frame.java`
- Create: `jni/java/org/red5/roqr/DeliveryMode.java`
- Create: `jni/java/org/red5/roqr/MessageListener.java`
- Modify: `jni/src/roqr_jni.cpp` (client native methods + callback bridge)
- Modify: `jni/CMakeLists.txt` (add the new Java sources to the jar)
- Modify: `tests/jni/` (integration test + CMake)
- Test: `tests/jni/RoqrClientTest.java`

**Interfaces:**
- Consumes: the C FFI client functions (Task 2); `JavaVM*` from Task 4.
- Produces:
  - `org.red5.roqr.DeliveryMode` enum `{ STREAM, DATAGRAM, AUTO }` with an `int nativeValue()` (0/1/2).
  - `org.red5.roqr.Frame` value class: fields `long flowId, timestamp, messageStreamId, chunkStreamId; int messageType; byte[] payload;` with a constructor and getters.
  - `org.red5.roqr.MessageListener` interface: `void onMessage(Frame frame); void onClosed(long appErrorCode);`
  - `org.red5.roqr.RoqrClient`: `RoqrClient()`, `void setListener(MessageListener)`, `boolean connect(String host, int port, boolean insecureSkipVerify)`, `boolean waitConnected(int timeoutMs)`, `boolean send(Frame frame, DeliveryMode mode)`, `void bindFlow(long)`, `void retireFlow(long)`, `void close(long appErrorCode)`, `boolean waitClosed(int timeoutMs)`, `void destroy()` (also `AutoCloseable`).
  - The JNI layer: RoqrClient holds a `long nativeHandle` (the `roqr_client*`) and a global ref to the listener; native callbacks attach the current thread, build a `Frame`, and call `onMessage`.

- [ ] **Step 1: Create the Java classes**

`jni/java/org/red5/roqr/DeliveryMode.java`:

```java
package org.red5.roqr;

/** RoQR delivery mode (draft s10). */
public enum DeliveryMode {
    STREAM(0),
    DATAGRAM(1),
    AUTO(2);

    private final int nativeValue;

    DeliveryMode(int nativeValue) {
        this.nativeValue = nativeValue;
    }

    public int nativeValue() {
        return nativeValue;
    }
}
```

`jni/java/org/red5/roqr/Frame.java`:

```java
package org.red5.roqr;

/** One RoQR frame: RTMP message metadata plus one message payload. */
public final class Frame {
    private final long flowId;
    private final long timestamp;
    private final int messageType;
    private final long messageStreamId;
    private final long chunkStreamId;
    private final byte[] payload;

    public Frame(long flowId, long timestamp, int messageType,
                 long messageStreamId, long chunkStreamId, byte[] payload) {
        this.flowId = flowId;
        this.timestamp = timestamp;
        this.messageType = messageType;
        this.messageStreamId = messageStreamId;
        this.chunkStreamId = chunkStreamId;
        this.payload = payload;
    }

    public long flowId() { return flowId; }
    public long timestamp() { return timestamp; }
    public int messageType() { return messageType; }
    public long messageStreamId() { return messageStreamId; }
    public long chunkStreamId() { return chunkStreamId; }
    public byte[] payload() { return payload; }
}
```

`jni/java/org/red5/roqr/MessageListener.java`:

```java
package org.red5.roqr;

/** Receives RoQR frames and close notifications. Callbacks fire on a
 * native (non-JVM-created) thread that the binding attaches for the call;
 * do not block in them. */
public interface MessageListener {
    void onMessage(Frame frame);

    void onClosed(long appErrorCode);
}
```

`jni/java/org/red5/roqr/RoqrClient.java`:

```java
package org.red5.roqr;

/** A RoQR client: connect to a RoQR server, send and receive frames. */
public final class RoqrClient implements AutoCloseable {
    static {
        System.loadLibrary("roqr-jni");
    }

    private long nativeHandle;
    private MessageListener listener;

    public RoqrClient() {
        nativeHandle = nativeCreate();
    }

    /** Set before connect(). */
    public void setListener(MessageListener listener) {
        this.listener = listener;
        nativeSetListener(nativeHandle, listener);
    }

    public boolean connect(String host, int port, boolean insecureSkipVerify) {
        return nativeConnect(nativeHandle, host, port, insecureSkipVerify);
    }

    public boolean waitConnected(int timeoutMs) {
        return nativeWaitConnected(nativeHandle, timeoutMs);
    }

    public boolean datagramsNegotiated() {
        return nativeDatagramsNegotiated(nativeHandle);
    }

    public boolean send(Frame frame, DeliveryMode mode) {
        return nativeSend(nativeHandle, frame.flowId(), frame.timestamp(),
                frame.messageType(), frame.messageStreamId(),
                frame.chunkStreamId(), frame.payload(), mode.nativeValue());
    }

    public void bindFlow(long flowId) { nativeBindFlow(nativeHandle, flowId); }
    public void retireFlow(long flowId) {
        nativeRetireFlow(nativeHandle, flowId);
    }

    public void close(long appErrorCode) {
        nativeClose(nativeHandle, appErrorCode);
    }

    public boolean waitClosed(int timeoutMs) {
        return nativeWaitClosed(nativeHandle, timeoutMs);
    }

    public void destroy() {
        if (nativeHandle != 0) {
            nativeDestroy(nativeHandle);
            nativeHandle = 0;
        }
    }

    @Override
    public void close() {
        destroy();
    }

    private static native long nativeCreate();
    private static native void nativeSetListener(long h, MessageListener l);
    private static native boolean nativeConnect(long h, String host, int port,
                                                boolean insecure);
    private static native boolean nativeWaitConnected(long h, int timeoutMs);
    private static native boolean nativeDatagramsNegotiated(long h);
    private static native boolean nativeSend(long h, long flowId, long ts,
                                             int type, long msid, long csid,
                                             byte[] payload, int mode);
    private static native void nativeBindFlow(long h, long flowId);
    private static native void nativeRetireFlow(long h, long flowId);
    private static native void nativeClose(long h, long appErrorCode);
    private static native boolean nativeWaitClosed(long h, int timeoutMs);
    private static native void nativeDestroy(long h);
}
```

- [ ] **Step 2: Write the failing test `tests/jni/RoqrClientTest.java`**

The test needs a RoQR server to echo. Since Java can't easily start the C++ relay, the test launches the `roqr-relayd` binary as a subprocess (it exists from Plan 2/4) pointed at the cert fixture, then drives a RoqrClient against it.

```java
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.red5.roqr.DeliveryMode;
import org.red5.roqr.Frame;
import org.red5.roqr.MessageListener;
import org.red5.roqr.RoqrClient;

public class RoqrClientTest {
    public static void main(String[] args) throws Exception {
        // args: <relayd-binary> <cert-dir>
        String relayd = args[0];
        String certDir = args[1];
        int port = 45610;

        Process relay = new ProcessBuilder(relayd,
                "--cert", certDir + "/cert.pem",
                "--key", certDir + "/key.pem",
                "--port", Integer.toString(port),
                "--mode", "echo").inheritIO().start();
        Thread.sleep(700);

        try {
            final CountDownLatch got = new CountDownLatch(1);
            final byte[][] received = new byte[1][];
            RoqrClient client = new RoqrClient();
            client.setListener(new MessageListener() {
                public void onMessage(Frame f) {
                    received[0] = f.payload();
                    got.countDown();
                }
                public void onClosed(long code) {}
            });
            if (!client.connect("127.0.0.1", port, true)) fail("connect");
            if (!client.waitConnected(5000)) fail("waitConnected");

            byte[] payload = {0x17, 0x01, (byte) 0xAB};
            Frame f = new Frame(0, 100, 9, 1, 6, payload);
            if (!client.send(f, DeliveryMode.STREAM)) fail("send");

            if (!got.await(5, TimeUnit.SECONDS)) fail("no echo");
            if (received[0].length != 3 || received[0][2] != (byte) 0xAB) {
                fail("payload mismatch");
            }
            client.close(0);
            client.waitClosed(5000);
            client.destroy();
            System.out.println("PASS: RoqrClient echo round-trip");
        } finally {
            relay.destroy();
            relay.waitFor(5, TimeUnit.SECONDS);
        }
    }

    private static void fail(String why) {
        System.err.println("FAIL: " + why);
        System.exit(1);
    }
}
```

- [ ] **Step 3: Wire the new Java sources + test**

Add the four new `.java` files to the `add_jar(roqr-jar SOURCES ...)` list in `jni/CMakeLists.txt`.

In `tests/jni/CMakeLists.txt`, add the client test jar + CTest test, passing the relayd binary path and cert dir (the cert fixture `ROQR_TEST_CERT_DIR` and `roqr-relayd` are defined when tools+examples are on — guard on `TARGET roqr-relayd`):

```cmake
if(TARGET roqr-relayd)
  add_jar(roqr-jni-client-test
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/RoqrClientTest.java
    INCLUDE_JARS roqr-jar)
  get_target_property(ROQR_CLIENT_TEST_JAR roqr-jni-client-test JAR_FILE)
  add_dependencies(roqr-jni-client-test roqr-testcerts)

  add_test(
    NAME roqr-jni-client
    COMMAND ${Java_JAVA_EXECUTABLE}
      -Djava.library.path=$<TARGET_FILE_DIR:roqr-jni>
      -cp "${ROQR_JAR_FILE}:${ROQR_CLIENT_TEST_JAR}"
      RoqrClientTest $<TARGET_FILE:roqr-relayd> ${ROQR_TEST_CERT_DIR})
  set_tests_properties(roqr-jni-client PROPERTIES TIMEOUT 60)
endif()
```

- [ ] **Step 4: Run to verify RED**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to link (missing `Java_org_red5_roqr_RoqrClient_nativeCreate` etc.).

- [ ] **Step 5: Implement the JNI client bridge in `jni/src/roqr_jni.cpp`**

Add below the existing version function:

```cpp
#include <cstdint>
#include <cstring>

namespace {

// Attaches the current thread to the JVM if needed. On destruction detaches
// only if this scope performed the attach.
struct AttachedEnv {
    JNIEnv* env = nullptr;
    bool attached = false;

    AttachedEnv() {
        if (g_vm == nullptr) return;
        const jint rc = g_vm->GetEnv(reinterpret_cast<void**>(&env),
                                     JNI_VERSION_1_6);
        if (rc == JNI_EDETACHED) {
            if (g_vm->AttachCurrentThread(reinterpret_cast<void**>(&env),
                                          nullptr) == JNI_OK) {
                attached = true;
            } else {
                env = nullptr;
            }
        } else if (rc != JNI_OK) {
            env = nullptr;
        }
    }
    ~AttachedEnv() {
        if (attached) g_vm->DetachCurrentThread();
    }
};

// Per-client JNI state: the roqr_client and a global ref to the listener
// plus cached method ids and the Frame constructor.
struct JniClient {
    roqr_client* handle = nullptr;
    jobject listener = nullptr;      // global ref
    jmethodID on_message = nullptr;  // MessageListener.onMessage(Frame)
    jmethodID on_closed = nullptr;   // MessageListener.onClosed(long)
    jclass frame_class = nullptr;    // global ref
    jmethodID frame_ctor = nullptr;
};

void message_trampoline(const roqr_frame* f, void* user) {
    auto* jc = static_cast<JniClient*>(user);
    if (jc->listener == nullptr) return;
    AttachedEnv ae;
    if (ae.env == nullptr) return;
    JNIEnv* env = ae.env;

    jbyteArray payload = env->NewByteArray(static_cast<jsize>(f->payload_len));
    env->SetByteArrayRegion(
        payload, 0, static_cast<jsize>(f->payload_len),
        reinterpret_cast<const jbyte*>(f->payload));
    jobject frame = env->NewObject(
        jc->frame_class, jc->frame_ctor, static_cast<jlong>(f->flow_id),
        static_cast<jlong>(f->timestamp), static_cast<jint>(f->message_type),
        static_cast<jlong>(f->message_stream_id),
        static_cast<jlong>(f->chunk_stream_id), payload);
    env->CallVoidMethod(jc->listener, jc->on_message, frame);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(frame);
    env->DeleteLocalRef(payload);
}

void closed_trampoline(uint64_t code, void* user) {
    auto* jc = static_cast<JniClient*>(user);
    if (jc->listener == nullptr) return;
    AttachedEnv ae;
    if (ae.env == nullptr) return;
    ae.env->CallVoidMethod(jc->listener, jc->on_closed,
                           static_cast<jlong>(code));
    if (ae.env->ExceptionCheck()) ae.env->ExceptionClear();
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_org_red5_roqr_RoqrClient_nativeCreate(JNIEnv* /*env*/, jclass) {
    auto* jc = new JniClient();
    jc->handle = roqr_client_create();
    return reinterpret_cast<jlong>(jc);
}

JNIEXPORT void JNICALL Java_org_red5_roqr_RoqrClient_nativeSetListener(
    JNIEnv* env, jclass, jlong h, jobject listener) {
    auto* jc = reinterpret_cast<JniClient*>(h);
    if (jc->listener != nullptr) env->DeleteGlobalRef(jc->listener);
    jc->listener = env->NewGlobalRef(listener);

    jclass lc = env->GetObjectClass(listener);
    jc->on_message =
        env->GetMethodID(lc, "onMessage", "(Lorg/red5/roqr/Frame;)V");
    jc->on_closed = env->GetMethodID(lc, "onClosed", "(J)V");

    jclass fc = env->FindClass("org/red5/roqr/Frame");
    jc->frame_class = static_cast<jclass>(env->NewGlobalRef(fc));
    jc->frame_ctor = env->GetMethodID(jc->frame_class, "<init>", "(JJIJJ[B)V");

    roqr_client_set_on_message(jc->handle, message_trampoline, jc);
    roqr_client_set_on_closed(jc->handle, closed_trampoline, jc);
}

JNIEXPORT jboolean JNICALL Java_org_red5_roqr_RoqrClient_nativeConnect(
    JNIEnv* env, jclass, jlong h, jstring host, jint port, jboolean insecure) {
    auto* jc = reinterpret_cast<JniClient*>(h);
    const char* chost = env->GetStringUTFChars(host, nullptr);
    const roqr_error rc = roqr_client_connect(
        jc->handle, chost, static_cast<uint16_t>(port), insecure ? 1 : 0);
    env->ReleaseStringUTFChars(host, chost);
    return rc == ROQR_OK ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_org_red5_roqr_RoqrClient_nativeWaitConnected(
    JNIEnv*, jclass, jlong h, jint timeout_ms) {
    auto* jc = reinterpret_cast<JniClient*>(h);
    return roqr_client_wait_connected(jc->handle, timeout_ms) ? JNI_TRUE
                                                              : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_red5_roqr_RoqrClient_nativeDatagramsNegotiated(JNIEnv*, jclass,
                                                        jlong h) {
    auto* jc = reinterpret_cast<JniClient*>(h);
    return roqr_client_datagrams_negotiated(jc->handle) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_org_red5_roqr_RoqrClient_nativeSend(
    JNIEnv* env, jclass, jlong h, jlong flow_id, jlong ts, jint type,
    jlong msid, jlong csid, jbyteArray payload, jint mode) {
    auto* jc = reinterpret_cast<JniClient*>(h);
    const jsize len = env->GetArrayLength(payload);
    std::vector<uint8_t> bytes(static_cast<size_t>(len));
    env->GetByteArrayRegion(payload, 0, len,
                            reinterpret_cast<jbyte*>(bytes.data()));
    roqr_frame f{};
    f.flow_id = static_cast<uint64_t>(flow_id);
    f.timestamp = static_cast<uint64_t>(ts);
    f.message_type = static_cast<uint8_t>(type);
    f.message_stream_id = static_cast<uint64_t>(msid);
    f.chunk_stream_id = static_cast<uint64_t>(csid);
    f.payload = bytes.data();
    f.payload_len = bytes.size();
    return roqr_client_send(jc->handle, &f,
                            static_cast<roqr_delivery_mode>(mode)) == ROQR_OK
               ? JNI_TRUE
               : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_red5_roqr_RoqrClient_nativeBindFlow(
    JNIEnv*, jclass, jlong h, jlong flow_id) {
    roqr_client_bind_flow(reinterpret_cast<JniClient*>(h)->handle,
                          static_cast<uint64_t>(flow_id));
}

JNIEXPORT void JNICALL Java_org_red5_roqr_RoqrClient_nativeRetireFlow(
    JNIEnv*, jclass, jlong h, jlong flow_id) {
    roqr_client_retire_flow(reinterpret_cast<JniClient*>(h)->handle,
                            static_cast<uint64_t>(flow_id));
}

JNIEXPORT void JNICALL Java_org_red5_roqr_RoqrClient_nativeClose(
    JNIEnv*, jclass, jlong h, jlong code) {
    roqr_client_close(reinterpret_cast<JniClient*>(h)->handle,
                      static_cast<uint64_t>(code));
}

JNIEXPORT jboolean JNICALL Java_org_red5_roqr_RoqrClient_nativeWaitClosed(
    JNIEnv*, jclass, jlong h, jint timeout_ms) {
    return roqr_client_wait_closed(reinterpret_cast<JniClient*>(h)->handle,
                                   timeout_ms)
               ? JNI_TRUE
               : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_red5_roqr_RoqrClient_nativeDestroy(
    JNIEnv* env, jclass, jlong h) {
    auto* jc = reinterpret_cast<JniClient*>(h);
    roqr_client_destroy(jc->handle);  // joins the network thread first
    if (jc->listener != nullptr) env->DeleteGlobalRef(jc->listener);
    if (jc->frame_class != nullptr) env->DeleteGlobalRef(jc->frame_class);
    delete jc;
}

}  // extern "C"
```

(Add `#include <vector>` at the top.)

**Ordering note:** `roqr_client_destroy` internally destroys the `roqr::quic::Client`, which joins the network thread — so no callback trampoline can run after `nativeDestroy` deletes the global refs. This is why the destroy order (destroy handle first, then delete refs) is correct and race-free.

- [ ] **Step 6: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev -R "roqr-jni" --output-on-failure`
Expected: both `roqr-jni-smoke` and `roqr-jni-client` PASS. Full suite: 145 prior + 1 (client) = 146. `--repeat until-fail:2` on `roqr-jni-client`.

- [ ] **Step 7: Commit**

```bash
git add jni tests
git commit -m "Add JNI client with native-thread callbacks"
```

---

### Task 6: JNI gateways — IngestGateway and EgressGateway Java classes

**Files:**
- Create: `jni/java/org/red5/roqr/IngestGateway.java`
- Create: `jni/java/org/red5/roqr/EgressGateway.java`
- Modify: `jni/src/roqr_jni.cpp` (ingest/egress native methods)
- Modify: `jni/CMakeLists.txt` (jar sources), `tests/jni/CMakeLists.txt`
- Test: `tests/jni/RoqrGatewayTest.java`

**Interfaces:**
- Consumes: the FFI gateway functions (Task 3).
- Produces:
  - `org.red5.roqr.IngestGateway`: `IngestGateway()`, `boolean start(int rtmpPort, String roqrHost, int roqrPort, boolean insecure)`, `boolean waitPublishing(int timeoutMs)`, `void stop()`, `void destroy()` / `AutoCloseable`.
  - `org.red5.roqr.EgressGateway`: `EgressGateway()`, `boolean start(int rtmpPort, String roqrHost, int roqrPort, String stream, boolean insecure)`, `boolean waitPlaying(int timeoutMs)`, `void stop()`, `void destroy()` / `AutoCloseable`.

- [ ] **Step 1: Create the Java classes**

`jni/java/org/red5/roqr/IngestGateway.java`:

```java
package org.red5.roqr;

/** Accepts an RTMP publisher and re-originates it over RoQR. */
public final class IngestGateway implements AutoCloseable {
    static {
        System.loadLibrary("roqr-jni");
    }

    private long nativeHandle;

    public IngestGateway() {
        nativeHandle = nativeCreate();
    }

    /** True if the RTMP listener started. Not a guarantee the RoQR server
     * leg is live; call waitPublishing to confirm the end-to-end path. */
    public boolean start(int rtmpPort, String roqrHost, int roqrPort,
                         boolean insecureSkipVerify) {
        return nativeStart(nativeHandle, rtmpPort, roqrHost, roqrPort,
                insecureSkipVerify);
    }

    public boolean waitPublishing(int timeoutMs) {
        return nativeWaitPublishing(nativeHandle, timeoutMs);
    }

    public void stop() { nativeStop(nativeHandle); }

    public void destroy() {
        if (nativeHandle != 0) {
            nativeDestroy(nativeHandle);
            nativeHandle = 0;
        }
    }

    @Override
    public void close() { destroy(); }

    private static native long nativeCreate();
    private static native boolean nativeStart(long h, int rtmpPort,
                                              String roqrHost, int roqrPort,
                                              boolean insecure);
    private static native boolean nativeWaitPublishing(long h, int timeoutMs);
    private static native void nativeStop(long h);
    private static native void nativeDestroy(long h);
}
```

`jni/java/org/red5/roqr/EgressGateway.java`:

```java
package org.red5.roqr;

/** Plays a RoQR stream and serves it to an RTMP player. */
public final class EgressGateway implements AutoCloseable {
    static {
        System.loadLibrary("roqr-jni");
    }

    private long nativeHandle;

    public EgressGateway() {
        nativeHandle = nativeCreate();
    }

    public boolean start(int rtmpPort, String roqrHost, int roqrPort,
                         String streamName, boolean insecureSkipVerify) {
        return nativeStart(nativeHandle, rtmpPort, roqrHost, roqrPort,
                streamName, insecureSkipVerify);
    }

    public boolean waitPlaying(int timeoutMs) {
        return nativeWaitPlaying(nativeHandle, timeoutMs);
    }

    public void stop() { nativeStop(nativeHandle); }

    public void destroy() {
        if (nativeHandle != 0) {
            nativeDestroy(nativeHandle);
            nativeHandle = 0;
        }
    }

    @Override
    public void close() { destroy(); }

    private static native long nativeCreate();
    private static native boolean nativeStart(long h, int rtmpPort,
                                              String roqrHost, int roqrPort,
                                              String streamName,
                                              boolean insecure);
    private static native boolean nativeWaitPlaying(long h, int timeoutMs);
    private static native void nativeStop(long h);
    private static native void nativeDestroy(long h);
}
```

- [ ] **Step 2: Write the failing test `tests/jni/RoqrGatewayTest.java`**

Launches `roqr-relayd --mode media` as a subprocess, starts a JNI IngestGateway and EgressGateway, drives an RTMP publish/play... which Java can't easily do without an RTMP client. Instead the test asserts the JNI gateway LIFECYCLE against a live media relay: ingest starts and (with no publisher) reports not-publishing within a short timeout; egress starts and reports playing (it connects+plays at start). This exercises the JNI ingest/egress start/wait/stop/destroy path end to end without an RTMP client in Java.

```java
import java.util.concurrent.TimeUnit;
import org.red5.roqr.EgressGateway;
import org.red5.roqr.IngestGateway;

public class RoqrGatewayTest {
    public static void main(String[] args) throws Exception {
        String relayd = args[0];
        String certDir = args[1];
        int relayPort = 45612;

        Process relay = new ProcessBuilder(relayd,
                "--cert", certDir + "/cert.pem",
                "--key", certDir + "/key.pem",
                "--port", Integer.toString(relayPort),
                "--mode", "media").inheritIO().start();
        Thread.sleep(700);
        try {
            IngestGateway ingest = new IngestGateway();
            if (!ingest.start(45613, "127.0.0.1", relayPort, true)) {
                fail("ingest start");
            }
            // No RTMP publisher connected -> not publishing yet.
            if (ingest.waitPublishing(500)) fail("unexpected publishing");

            EgressGateway egress = new EgressGateway();
            if (!egress.start(45614, "127.0.0.1", relayPort, "cam", true)) {
                fail("egress start");
            }
            // Egress connects+plays at start -> playing becomes true.
            if (!egress.waitPlaying(5000)) fail("egress not playing");

            ingest.stop();
            ingest.destroy();
            egress.stop();
            egress.destroy();
            System.out.println("PASS: JNI gateway lifecycle");
        } finally {
            relay.destroy();
            relay.waitFor(5, TimeUnit.SECONDS);
        }
    }

    private static void fail(String why) {
        System.err.println("FAIL: " + why);
        System.exit(1);
    }
}
```

- [ ] **Step 3: Wire sources + test**

Add `IngestGateway.java`/`EgressGateway.java` to the `roqr-jar` sources in `jni/CMakeLists.txt`. Add the gateway test to `tests/jni/CMakeLists.txt` inside the `if(TARGET roqr-relayd)` block, mirroring the client test (jar via `add_jar` INCLUDE_JARS roqr-jar; `add_test` passing relayd + cert dir; TIMEOUT 60).

- [ ] **Step 4: Run to verify RED**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to link (missing `Java_org_red5_roqr_IngestGateway_nativeCreate`).

- [ ] **Step 5: Implement the gateway JNI methods in `jni/src/roqr_jni.cpp`**

Add `#include "roqr/roqr_rtmp.h"` inside the existing `extern "C" { #include ... }` block, and add:

```cpp
extern "C" {

JNIEXPORT jlong JNICALL
Java_org_red5_roqr_IngestGateway_nativeCreate(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(roqr_ingest_create());
}

JNIEXPORT jboolean JNICALL Java_org_red5_roqr_IngestGateway_nativeStart(
    JNIEnv* env, jclass, jlong h, jint rtmp_port, jstring host, jint roqr_port,
    jboolean insecure) {
    const char* chost = env->GetStringUTFChars(host, nullptr);
    const roqr_error rc = roqr_ingest_start(
        reinterpret_cast<roqr_ingest*>(h), static_cast<uint16_t>(rtmp_port),
        chost, static_cast<uint16_t>(roqr_port), insecure ? 1 : 0);
    env->ReleaseStringUTFChars(host, chost);
    return rc == ROQR_OK ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_red5_roqr_IngestGateway_nativeWaitPublishing(JNIEnv*, jclass, jlong h,
                                                      jint timeout_ms) {
    return roqr_ingest_wait_publishing(reinterpret_cast<roqr_ingest*>(h),
                                       timeout_ms)
               ? JNI_TRUE
               : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_red5_roqr_IngestGateway_nativeStop(
    JNIEnv*, jclass, jlong h) {
    roqr_ingest_stop(reinterpret_cast<roqr_ingest*>(h));
}

JNIEXPORT void JNICALL Java_org_red5_roqr_IngestGateway_nativeDestroy(
    JNIEnv*, jclass, jlong h) {
    roqr_ingest_destroy(reinterpret_cast<roqr_ingest*>(h));
}

JNIEXPORT jlong JNICALL
Java_org_red5_roqr_EgressGateway_nativeCreate(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(roqr_egress_create());
}

JNIEXPORT jboolean JNICALL Java_org_red5_roqr_EgressGateway_nativeStart(
    JNIEnv* env, jclass, jlong h, jint rtmp_port, jstring host, jint roqr_port,
    jstring stream, jboolean insecure) {
    const char* chost = env->GetStringUTFChars(host, nullptr);
    const char* cstream = env->GetStringUTFChars(stream, nullptr);
    const roqr_error rc = roqr_egress_start(
        reinterpret_cast<roqr_egress*>(h), static_cast<uint16_t>(rtmp_port),
        chost, static_cast<uint16_t>(roqr_port), cstream, insecure ? 1 : 0);
    env->ReleaseStringUTFChars(host, chost);
    env->ReleaseStringUTFChars(stream, cstream);
    return rc == ROQR_OK ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_org_red5_roqr_EgressGateway_nativeWaitPlaying(
    JNIEnv*, jclass, jlong h, jint timeout_ms) {
    return roqr_egress_wait_playing(reinterpret_cast<roqr_egress*>(h),
                                    timeout_ms)
               ? JNI_TRUE
               : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_red5_roqr_EgressGateway_nativeStop(
    JNIEnv*, jclass, jlong h) {
    roqr_egress_stop(reinterpret_cast<roqr_egress*>(h));
}

JNIEXPORT void JNICALL Java_org_red5_roqr_EgressGateway_nativeDestroy(
    JNIEnv*, jclass, jlong h) {
    roqr_egress_destroy(reinterpret_cast<roqr_egress*>(h));
}

}  // extern "C"
```

- [ ] **Step 6: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev -R "roqr-jni" --output-on-failure`
Expected: `roqr-jni-smoke`, `roqr-jni-client`, `roqr-jni-gateway` all PASS. Full suite: 146 prior + 1 = 147. `--repeat until-fail:2` on `roqr-jni-gateway`.

- [ ] **Step 7: Commit**

```bash
git add jni tests
git commit -m "Add JNI ingest and egress gateway bindings"
```

---

### Task 7: Java sample apps, Android NDK build config, and docs

**Files:**
- Create: `examples/java/PublishSample.java`
- Create: `examples/java/PlaySample.java`
- Create: `examples/java/README.md`
- Create: `cmake/android-jni.md` (Android NDK build guide — authorized doc)
- Modify: `jni/CMakeLists.txt` (a `roqr-samples` jar target for the samples)
- Modify: `README.md` (root: add an FFI/JNI section)

**Interfaces:**
- Consumes: the JNI classes (Tasks 5-6).
- Produces: two runnable Java sample mains; an Android NDK cross-compile guide; a samples jar target; README updates. No new CTest test (samples need a running server; they are documented, not CI-run).

- [ ] **Step 1: Create `examples/java/PublishSample.java`**

```java
import org.red5.roqr.IngestGateway;

/** Bridges an RTMP publisher into a RoQR server.
 *  Usage: PublishSample <rtmpPort> <roqrHost> <roqrPort> */
public final class PublishSample {
    public static void main(String[] args) throws Exception {
        if (args.length < 3) {
            System.err.println(
                "usage: PublishSample <rtmpPort> <roqrHost> <roqrPort>");
            System.exit(2);
        }
        int rtmpPort = Integer.parseInt(args[0]);
        String host = args[1];
        int roqrPort = Integer.parseInt(args[2]);

        try (IngestGateway ingest = new IngestGateway()) {
            if (!ingest.start(rtmpPort, host, roqrPort, true)) {
                System.err.println("failed to start ingest");
                System.exit(1);
            }
            System.out.printf(
                "PublishSample: publish RTMP to rtmp://127.0.0.1:%d/live/<name>%n",
                rtmpPort);
            System.out.println("Ctrl-C to stop.");
            Thread.currentThread().join();
        }
    }
}
```

- [ ] **Step 2: Create `examples/java/PlaySample.java`**

```java
import org.red5.roqr.EgressGateway;

/** Serves a RoQR stream to an RTMP player.
 *  Usage: PlaySample <rtmpPort> <roqrHost> <roqrPort> <stream> */
public final class PlaySample {
    public static void main(String[] args) throws Exception {
        if (args.length < 4) {
            System.err.println(
                "usage: PlaySample <rtmpPort> <roqrHost> <roqrPort> <stream>");
            System.exit(2);
        }
        int rtmpPort = Integer.parseInt(args[0]);
        String host = args[1];
        int roqrPort = Integer.parseInt(args[2]);
        String stream = args[3];

        try (EgressGateway egress = new EgressGateway()) {
            if (!egress.start(rtmpPort, host, roqrPort, stream, true)) {
                System.err.println("failed to start egress");
                System.exit(1);
            }
            if (!egress.waitPlaying(5000)) {
                System.err.println("warning: RoQR play not confirmed");
            }
            System.out.printf(
                "PlaySample: play with ffplay rtmp://127.0.0.1:%d/live/%s%n",
                rtmpPort, stream);
            System.out.println("Ctrl-C to stop.");
            Thread.currentThread().join();
        }
    }
}
```

- [ ] **Step 3: Add a samples jar target and README**

`examples/java/README.md`:

```markdown
# libroqr Java samples

Build the JNI bindings and the samples:

```
eval "$(scripts/setup_picoquic_deps.sh)"
cmake --preset dev            # ROQR_BUILD_JNI=ON in the dev preset
cmake --build --preset dev
```

Run (point java at the built native lib and jars):

```
JNI_DIR=build/dev/jni
JAR=$(find build/dev -name roqr.jar)
SAMPLES=$(find build/dev -name roqr-samples.jar)

# Publisher gateway on :1935 -> RoQR server 127.0.0.1:4443
java -Djava.library.path=$JNI_DIR -cp "$JAR:$SAMPLES" \
    PublishSample 1935 127.0.0.1 4443
# then: ffmpeg ... -f flv rtmp://127.0.0.1:1935/live/cam

# Player gateway on :1936 <- RoQR server, stream "cam"
java -Djava.library.path=$JNI_DIR -cp "$JAR:$SAMPLES" \
    PlaySample 1936 127.0.0.1 4443 cam
# then: ffplay rtmp://127.0.0.1:1936/live/cam
```

A `roqr-relayd --mode media` (or any RoQR server) must be running at the
RoQR host/port.
```

In `jni/CMakeLists.txt`, add a samples jar (only when the samples dir is used — keep it simple, compile against roqr-jar):

```cmake
add_jar(roqr-samples
  SOURCES
    ${CMAKE_SOURCE_DIR}/examples/java/PublishSample.java
    ${CMAKE_SOURCE_DIR}/examples/java/PlaySample.java
  INCLUDE_JARS roqr-jar
  OUTPUT_NAME roqr-samples)
```

- [ ] **Step 4: Create the Android NDK build guide `cmake/android-jni.md`**

```markdown
# Building the RoQR JNI library for Android (NDK)

The `roqr-jni` shared library and its dependencies (roqr-core, roqr-quic,
roqr-rtmp, roqr-gateway, roqr-ffi, picoquic, picotls, OpenSSL) are all
C/C++ and cross-compile for Android with the NDK toolchain. The Java
classes in `jni/java/org/red5/roqr` are platform-independent and go into
your Android app or an AAR unchanged.

## Prerequisites

- Android NDK r25+ (`ANDROID_NDK_HOME` set).
- OpenSSL built for the target ABI. This repo's sibling `openssl-android`
  tree provides prebuilt static libs; point `OPENSSL_ROOT_DIR` at the ABI
  you are building.
- picoquic/picotls source (fetched by `scripts/setup_picoquic_deps.sh`);
  they build under the NDK toolchain via the same source-tree mechanism
  `FindPicoquic.cmake` uses.

## Configure

```
export ANDROID_ABI=arm64-v8a          # or armeabi-v7a, x86_64
export ANDROID_PLATFORM=android-24

eval "$(scripts/setup_picoquic_deps.sh)"

cmake -S . -B build/android-$ANDROID_ABI \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=$ANDROID_ABI \
  -DANDROID_PLATFORM=$ANDROID_PLATFORM \
  -DOPENSSL_ROOT_DIR=/path/to/openssl-android/$ANDROID_ABI \
  -DROQR_BUILD_QUIC=ON -DROQR_BUILD_RTMP=ON \
  -DROQR_BUILD_EXAMPLES=ON -DROQR_BUILD_FFI=ON \
  -DROQR_BUILD_JNI=ON -DROQR_BUILD_TESTS=OFF -DROQR_BUILD_TOOLS=OFF

cmake --build build/android-$ANDROID_ABI --target roqr-jni
```

`find_package(JNI)` is not used on Android — the NDK provides `jni.h` on the
default include path, so `roqr-jni` compiles against it. If your CMake
version's `FindJNI` misbehaves under the NDK, the `jni/CMakeLists.txt`
`target_include_directories(... ${JNI_INCLUDE_DIRS})` line is a no-op there
(the NDK sysroot already has `jni.h`); guard it with
`if(NOT ANDROID)` if configure complains.

## Package

Copy `build/android-<abi>/jni/libroqr-jni.so` into
`app/src/main/jniLibs/<abi>/`, add the `org.red5.roqr` Java sources (or the
`roqr.jar`) to your app, and call `System.loadLibrary("roqr-jni")` (already
done in the static initializers).

## ABI note

Build one `libroqr-jni.so` per ABI you ship. The Java API is identical
across ABIs.
```

**Implementation note:** if `jni/CMakeLists.txt`'s `${JNI_INCLUDE_DIRS}` line breaks the Android configure, wrap it as the guide says: `if(NOT ANDROID) target_include_directories(roqr-jni PRIVATE ${JNI_INCLUDE_DIRS}) endif()` and link `${JNI_LIBRARIES}` only `if(NOT ANDROID)`. Make that guard change now so the documented Android build works.

- [ ] **Step 5: Update the root `README.md`** — add a section after the layout:

```markdown
## Language bindings

- `ffi/` a C ABI (`roqr.h` for the client, `roqr_rtmp.h` for the ingest/egress
  gateways) in `libroqr-ffi.so`.
- `jni/` JNI bindings (`org.red5.roqr`) in `libroqr-jni.so` + `roqr.jar`,
  built when `-DROQR_BUILD_JNI=ON` and a JDK is present.
- `examples/java/` Java publish/play samples (see `examples/java/README.md`).
- Android: see `cmake/android-jni.md` for the NDK cross-compile.

Native callbacks (`MessageListener`) fire on a native thread the binding
attaches to the JVM for the call; do not block in them.
```

- [ ] **Step 6: Verify and build the samples jar**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: full suite still 147 (no new CTest test; the samples jar builds as part of the build). Confirm `find build/dev -name roqr-samples.jar` exists and `find build/dev -name roqr.jar` exists.

- [ ] **Step 7: Commit**

```bash
git add examples jni cmake README.md
git commit -m "Add Java samples, Android NDK build guide, and binding docs"
```

---

## Completion Criteria

- `ctest --preset dev` green: 147 registered (136 baseline + 11 new: ffi smoke 3, ffi client 3, ffi gateway 2, jni smoke 1, jni client 1, jni gateway 1), `--repeat until-fail:2` stable on the JNI + FFI integration tests, warning-clean.
- `libroqr-ffi.so` exposes the documented C ABI; a C program can publish/subscribe frames and drive the gateways.
- `libroqr-jni.so` + `roqr.jar` let a Java program (`RoqrClient`, `IngestGateway`, `EgressGateway`) do the same; native callbacks are delivered on attached threads without crashing the JVM.
- The Android NDK guide describes a complete cross-compile; the `jni/CMakeLists.txt` Android guard is in place.
- Core-only build (QUIC=OFF, FFI/JNI off) still works; JNI skips cleanly when no JDK is found.
- Deferred backlog carried forward in the ledger (egress blocking send + reconnect, TSAN CI, test #63 flake, ack wrap, Reader bounds asserts/fuzzing, outbound_queue bound, Listener reaping) for the final review to triage.

## Project Completion

Plan 5 is the last plan. After merge, libroqr implements the full RoQR draft with: a sans-I/O core, a picoquic transport, an RTMP/AMF/E-RTMP gateway module, a test relay, ffmpeg-validated gateways, and C/JNI/Java bindings — the deliverable the original request described (C++20 roqr with FFI, JNI, and native app examples connecting to a RoQR server as bidirectional full-duplex clients, RTMP flowing over QUIC).
