# libroqr Plan 2: picoquic Client Transport and Test Relay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The `roqr-quic` picoquic-based client transport (threaded, callback API) and the `roqr-relayd` test server, proven by in-process loopback integration tests over real QUIC.

**Architecture:** `roqr::quic::Client` wraps a picoquic client context plus one background network thread (`picoquic_start_network_thread`); all picoquic calls happen on that thread, and the app thread communicates through a mutex-protected outbound queue plus `picoquic_wake_up_network_thread`. Receive side feeds per-stream `FrameDecoder`s and `datagram_decode` from roqr-core, consults `FlowTable`, and delivers via callbacks on the network thread. `roqr::relayd::Server` is the same pattern in server mode with Echo and Relay behaviors, built as a library (`roqr-relayd-lib`) so integration tests embed it in-process, plus a thin CLI binary.

**Tech Stack:** C++20, picoquic (pinned commit) + picotls + OpenSSL, Catch2 v3, CTest. picoquic acquisition mirrors moq5: setup script clones pinned SHAs, `FindPicoquic.cmake` resolves installed package or source dir.

**Spec:** `docs/superpowers/specs/2026-07-04-libroqr-design.md`. Draft: `../roqr/draft-gregoire-rtmp-over-quic.txt` (s4 ALPN, s5 flows, s7.4/s7.5 carriage, s8 loss, s10 mode choice, s12 errors, Table 2).

## Global Constraints

- C++20; namespace `roqr::quic::` for the transport, `roqr::relayd::` for the server; include layouts `quic/include/roqr/quic/*.hpp`, `tools/relayd/include/roqr/relayd/*.hpp`.
- ALPN token is exactly `roqr` (draft s4).
- All picoquic API calls MUST happen on the network thread (picoquic is not thread-safe). App-thread entry points only mutate roqr-owned state under a mutex and call `picoquic_wake_up_network_thread`.
- picoquic pinned commit `55b473e207e436d06ea9a2895cc1fc555d42c81c`; picotls pinned `7c32032f91449d695b24b82955f20d04d47e6cff` (proven pair from moq5).
- **API-drift rule:** the picoquic function names and signatures in this plan were verified against the moq5 adapter using the same pinned commit. If a call in this plan does not compile against the pinned headers, adapt minimally to the pinned API and record the exact substitution in your task report; if the *behavior* available differs (not just a name), stop and report BLOCKED.
- Warning flags: new library targets get `$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>` like roqr-core; builds must stay warning-clean.
- Commit messages: plain imperative, no emoji, no Claude tagline, no Co-Authored-By. TDD per task.
- Integration tests use loopback (`127.0.0.1`), fixed ports in the 45550-45599 range (one port per test case), server started in-process, and MUST bound all waits (no infinite loops; use the wait helpers with timeouts, fail the test on timeout).
- Build commands: `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`. The dev preset gains `ROQR_BUILD_QUIC=ON` and `ROQR_BUILD_TOOLS=ON`; both options default OFF at the root so roqr-core alone still builds without picoquic.
- **Spec deferral (documented):** the spec's `on_datagram_gap_hint` callback is NOT in this plan. Gap detection is a per-flow RTMP-timestamp heuristic that belongs with the media-aware gap-recovery logic; it lands in Plan 4 alongside the egress gateway that consumes it.

---

### Task 1: picoquic dependency acquisition and quic build skeleton

**Files:**
- Create: `scripts/setup_picoquic_deps.sh`
- Create: `cmake/FindPicoquic.cmake`
- Create: `quic/CMakeLists.txt`
- Create: `quic/include/roqr/quic/context.hpp`
- Create: `quic/src/context.cpp`
- Modify: `CMakeLists.txt` (root: options + subdirs)
- Modify: `CMakePresets.json` (dev preset enables quic/tools)
- Modify: `tests/CMakeLists.txt` (new `roqr-quic-tests` target, gated)
- Test: `tests/quic/context_test.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks (build-level only).
- Produces:
  - CMake option `ROQR_BUILD_QUIC` (OFF default), `ROQR_BUILD_TOOLS` (OFF default); target `roqr-quic` linking `roqr-core` and `picoquic::picoquic-core`; test target `roqr-quic-tests`.
  - `roqr::quic::QuicContext` — RAII wrapper: `static std::unique_ptr<QuicContext> create_client(const std::string& alpn, bool insecure_skip_verify)`, `static std::unique_ptr<QuicContext> create_server(const std::string& alpn, const std::string& cert_file, const std::string& key_file, picoquic_stream_data_cb_fn cb, void* cb_ctx)`, `picoquic_quic_t* get()`, destructor calls `picoquic_free`.
  - Env convention: `eval "$(scripts/setup_picoquic_deps.sh)"` prints `ROQR_PICOQUIC_SOURCE_DIR=...` and `ROQR_PICOTLS_PREFIX=...` exports.

- [ ] **Step 1: Create `scripts/setup_picoquic_deps.sh`** (mode +x)

```bash
#!/usr/bin/env bash
# Clones and builds the pinned picoquic + picotls pair for source-tree builds.
# Usage: eval "$(scripts/setup_picoquic_deps.sh)"
# Prints export lines for ROQR_PICOQUIC_SOURCE_DIR and ROQR_PICOTLS_PREFIX.
set -euo pipefail

PICOQUIC_REPO="https://github.com/private-octopus/picoquic"
PICOQUIC_REF="55b473e207e436d06ea9a2895cc1fc555d42c81c"
PICOTLS_REPO="https://github.com/h2o/picotls.git"
PICOTLS_REF="7c32032f91449d695b24b82955f20d04d47e6cff"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="${ROOT}/.deps"
mkdir -p "${DEPS}"

fetch_ref() { # dir repo ref
    local dir="$1" repo="$2" ref="$3"
    if [ ! -d "${dir}/.git" ]; then
        git init -q "${dir}"
        git -C "${dir}" remote add origin "${repo}"
    fi
    if ! git -C "${dir}" cat-file -e "${ref}^{commit}" 2>/dev/null; then
        git -C "${dir}" fetch -q --depth 1 origin "${ref}"
    fi
    git -C "${dir}" checkout -q "${ref}"
}

fetch_ref "${DEPS}/picotls" "${PICOTLS_REPO}" "${PICOTLS_REF}" >&2
git -C "${DEPS}/picotls" submodule update --init --recursive -q >&2
if [ ! -f "${DEPS}/picotls/build/libpicotls-core.a" ]; then
    cmake -S "${DEPS}/picotls" -B "${DEPS}/picotls/build" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >&2
    cmake --build "${DEPS}/picotls/build" --parallel \
        --target picotls-core picotls-openssl picotls-minicrypto >&2
fi

fetch_ref "${DEPS}/picoquic" "${PICOQUIC_REPO}" "${PICOQUIC_REF}" >&2

echo "export ROQR_PICOQUIC_SOURCE_DIR=${DEPS}/picoquic"
echo "export ROQR_PICOTLS_PREFIX=${DEPS}/picotls/build"
```

- [ ] **Step 2: Create `cmake/FindPicoquic.cmake`**

```cmake
# FindPicoquic.cmake - provides picoquic::picoquic-core.
# Resolution order:
#   1. Target already defined by the enclosing project.
#   2. Installed config package: find_package(picoquic CONFIG).
#   3. Source tree: -DROQR_PICOQUIC_SOURCE_DIR=/path (env var of the same
#      name is honored), with picotls resolved via ROQR_PICOTLS_PREFIX.

if(TARGET picoquic::picoquic-core)
    set(Picoquic_FOUND TRUE)
    return()
endif()

find_package(picoquic CONFIG QUIET)
if(picoquic_FOUND)
    if(NOT TARGET picoquic::picoquic-core AND TARGET picoquic-core)
        add_library(picoquic::picoquic-core ALIAS picoquic-core)
    endif()
    set(Picoquic_FOUND TRUE)
    return()
endif()

if(NOT ROQR_PICOQUIC_SOURCE_DIR AND DEFINED ENV{ROQR_PICOQUIC_SOURCE_DIR})
    set(ROQR_PICOQUIC_SOURCE_DIR "$ENV{ROQR_PICOQUIC_SOURCE_DIR}")
endif()
if(NOT ROQR_PICOTLS_PREFIX AND DEFINED ENV{ROQR_PICOTLS_PREFIX})
    set(ROQR_PICOTLS_PREFIX "$ENV{ROQR_PICOTLS_PREFIX}")
endif()

if(ROQR_PICOQUIC_SOURCE_DIR)
    if(NOT ROQR_PICOTLS_PREFIX)
        set(ROQR_PICOTLS_PREFIX "${ROQR_PICOQUIC_SOURCE_DIR}/../picotls/build")
    endif()
    # picoquic's CMake finds picotls via PTLS_* hints.
    set(PTLS_PREFIX "${ROQR_PICOTLS_PREFIX}" CACHE PATH "" FORCE)
    list(APPEND CMAKE_PREFIX_PATH "${ROQR_PICOTLS_PREFIX}")

    set(BUILD_DEMO OFF CACHE BOOL "" FORCE)
    set(BUILD_PQBENCH OFF CACHE BOOL "" FORCE)
    set(BUILD_LOGLIB ON CACHE BOOL "" FORCE)
    set(BUILD_HTTP OFF CACHE BOOL "" FORCE)
    set(picoquic_BUILD_TESTS OFF CACHE BOOL "" FORCE)

    add_subdirectory("${ROQR_PICOQUIC_SOURCE_DIR}" picoquic-build EXCLUDE_FROM_ALL)

    if(NOT TARGET picoquic::picoquic-core AND TARGET picoquic-core)
        add_library(picoquic::picoquic-core ALIAS picoquic-core)
    endif()
    if(TARGET picoquic-log)
        set_property(TARGET picoquic-core APPEND PROPERTY
            INTERFACE_LINK_LIBRARIES picoquic-log)
    endif()
    set(Picoquic_FOUND TRUE)
    return()
endif()

set(Picoquic_FOUND FALSE)
if(Picoquic_FIND_REQUIRED)
    message(FATAL_ERROR
        "picoquic not found. Install it, or run: eval \"$(scripts/setup_picoquic_deps.sh)\" and re-configure.")
endif()
```

- [ ] **Step 3: Modify root `CMakeLists.txt`** — full new content:

```cmake
cmake_minimum_required(VERSION 3.24)

project(libroqr
  VERSION 0.1.0
  DESCRIPTION "RTMP over QUIC (RoQR) client library"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(ROQR_BUILD_TESTS "Build unit tests" ON)
option(ROQR_BUILD_QUIC "Build the picoquic transport (needs picoquic)" OFF)
option(ROQR_BUILD_TOOLS "Build tools (roqr-relayd; needs ROQR_BUILD_QUIC)" OFF)

add_subdirectory(core)

if(ROQR_BUILD_QUIC)
  find_package(Picoquic REQUIRED)
  find_package(OpenSSL REQUIRED)
  add_subdirectory(quic)
endif()

if(ROQR_BUILD_TOOLS)
  if(NOT ROQR_BUILD_QUIC)
    message(FATAL_ERROR "ROQR_BUILD_TOOLS requires ROQR_BUILD_QUIC=ON")
  endif()
  add_subdirectory(tools/relayd)
endif()

if(ROQR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
```

- [ ] **Step 4: Modify `CMakePresets.json`** — add to the dev preset's `cacheVariables`:

```json
        "ROQR_BUILD_QUIC": "ON",
        "ROQR_BUILD_TOOLS": "ON"
```

(The release preset is unchanged; quic stays opt-in there.)

- [ ] **Step 5: Create `quic/include/roqr/quic/context.hpp`**

```cpp
#pragma once

#include <memory>
#include <string>

#include <picoquic.h>

namespace roqr::quic {

// RAII owner of a picoquic_quic_t. Configuration that must happen before
// any connection exists (verifier, ALPN) lives here.
class QuicContext {
public:
    static std::unique_ptr<QuicContext> create_client(
        const std::string& alpn, bool insecure_skip_verify);

    static std::unique_ptr<QuicContext> create_server(
        const std::string& alpn, const std::string& cert_file,
        const std::string& key_file, picoquic_stream_data_cb_fn cb,
        void* cb_ctx);

    ~QuicContext();
    QuicContext(const QuicContext&) = delete;
    QuicContext& operator=(const QuicContext&) = delete;

    picoquic_quic_t* get() { return quic_; }

private:
    explicit QuicContext(picoquic_quic_t* quic) : quic_(quic) {}
    picoquic_quic_t* quic_;
};

}  // namespace roqr::quic
```

- [ ] **Step 6: Create `quic/src/context.cpp`**

```cpp
#include "roqr/quic/context.hpp"

#include <picoquic_utils.h>

namespace roqr::quic {

std::unique_ptr<QuicContext> QuicContext::create_client(
    const std::string& alpn, bool insecure_skip_verify) {
    const uint64_t now = picoquic_current_time();
    picoquic_quic_t* quic = picoquic_create(
        1, nullptr, nullptr, nullptr, alpn.c_str(), nullptr, nullptr,
        nullptr, nullptr, nullptr, now, nullptr, nullptr, nullptr, 0);
    if (quic == nullptr) return nullptr;
    if (insecure_skip_verify) {
        picoquic_set_null_verifier(quic);
    }
    return std::unique_ptr<QuicContext>(new QuicContext(quic));
}

std::unique_ptr<QuicContext> QuicContext::create_server(
    const std::string& alpn, const std::string& cert_file,
    const std::string& key_file, picoquic_stream_data_cb_fn cb,
    void* cb_ctx) {
    const uint64_t now = picoquic_current_time();
    picoquic_quic_t* quic = picoquic_create(
        8, cert_file.c_str(), key_file.c_str(), nullptr, alpn.c_str(), cb,
        cb_ctx, nullptr, nullptr, nullptr, now, nullptr, nullptr, nullptr,
        0);
    if (quic == nullptr) return nullptr;
    return std::unique_ptr<QuicContext>(new QuicContext(quic));
}

QuicContext::~QuicContext() {
    if (quic_ != nullptr) picoquic_free(quic_);
}

}  // namespace roqr::quic
```

- [ ] **Step 7: Create `quic/CMakeLists.txt`**

```cmake
add_library(roqr-quic STATIC
  src/context.cpp
)

target_include_directories(roqr-quic PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(roqr-quic
  PUBLIC roqr-core picoquic::picoquic-core
  PRIVATE OpenSSL::SSL OpenSSL::Crypto
)

target_compile_features(roqr-quic PUBLIC cxx_std_20)
target_compile_options(roqr-quic PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
```

- [ ] **Step 8: Write the failing test `tests/quic/context_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include "roqr/quic/context.hpp"

using namespace roqr::quic;

TEST_CASE("client context creates and destroys cleanly") {
    auto ctx = QuicContext::create_client("roqr", true);
    REQUIRE(ctx != nullptr);
    CHECK(ctx->get() != nullptr);
}

TEST_CASE("server context requires readable cert and key") {
    auto ctx = QuicContext::create_server(
        "roqr", "/nonexistent/cert.pem", "/nonexistent/key.pem", nullptr,
        nullptr);
    // picoquic_create fails (returns null) when the cert cannot be loaded.
    CHECK(ctx == nullptr);
}
```

Append to `tests/CMakeLists.txt` (after the existing `catch_discover_tests`):

```cmake
if(ROQR_BUILD_QUIC)
  add_executable(roqr-quic-tests
    quic/context_test.cpp
  )
  target_link_libraries(roqr-quic-tests PRIVATE roqr-quic Catch2::Catch2WithMain)
  catch_discover_tests(roqr-quic-tests)
endif()
```

- [ ] **Step 9: Fetch deps, configure, verify RED then GREEN**

Run:
```bash
eval "$(scripts/setup_picoquic_deps.sh)"
cmake --preset dev
cmake --build --preset dev && ctest --preset dev
```
Expected: deps clone+build succeeds (needs network + OpenSSL dev headers; if either fails, report BLOCKED with the error); then all tests pass — 35 core + 2 new = 37. If `picoquic_create` returns non-null for the bad-cert server case, adapt the test to the pinned behavior (check `picoquic_create`'s actual failure mode, e.g. it may defer cert loading; if so assert non-null and note it in your report — do not delete the test).

- [ ] **Step 10: Commit**

```bash
git add scripts cmake CMakeLists.txt CMakePresets.json quic tests
git commit -m "Add picoquic dependency wiring and quic context skeleton"
```

---

### Task 2: Delivery-mode policy and outbound queue

**Files:**
- Create: `quic/include/roqr/quic/delivery.hpp`
- Create: `quic/src/delivery.cpp`
- Create: `quic/include/roqr/quic/outbound_queue.hpp`
- Modify: `quic/CMakeLists.txt` (add `src/delivery.cpp`)
- Modify: `tests/CMakeLists.txt` (add the two test files)
- Test: `tests/quic/delivery_test.cpp`, `tests/quic/outbound_queue_test.cpp`

**Interfaces:**
- Consumes: `roqr::Frame` from core.
- Produces:
  - `enum class roqr::quic::DeliveryMode { Stream, Datagram, Auto }`
  - `enum class roqr::quic::DatagramFallback { Stream, Drop }`
  - `enum class roqr::quic::ResolvedMode { Stream, Datagram, Dropped }`
  - `ResolvedMode roqr::quic::resolve_delivery(uint8_t message_type, DeliveryMode requested, bool datagram_negotiated, size_t encoded_size, size_t max_datagram_size, DatagramFallback fallback)` — draft s7.5/s10 policy.
  - `class roqr::quic::OutboundQueue` — thread-safe FIFO of `Outbound { roqr::Frame frame; DeliveryMode mode; }`: `void push(Outbound)`, `std::optional<Outbound> pop()`, `size_t size() const`.

- [ ] **Step 1: Write the failing tests**

`tests/quic/delivery_test.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "roqr/quic/delivery.hpp"

using namespace roqr::quic;

namespace {
constexpr size_t kMax = 1200;
}

TEST_CASE("Stream mode always resolves to stream") {
    CHECK(resolve_delivery(9, DeliveryMode::Stream, true, 100, kMax,
                           DatagramFallback::Stream) == ResolvedMode::Stream);
}

TEST_CASE("Auto sends non-media message types on stream") {
    // 20 = AMF0 Command, 18 = AMF0 Data, 4 = User Control: session
    // correctness traffic (draft s10) stays on streams even in Auto.
    for (uint8_t type : {1, 2, 3, 4, 5, 6, 15, 17, 18, 19, 20}) {
        CHECK(resolve_delivery(type, DeliveryMode::Auto, true, 100, kMax,
                               DatagramFallback::Stream) ==
              ResolvedMode::Stream);
    }
}

TEST_CASE("Auto sends fitting media in datagrams when negotiated") {
    for (uint8_t type : {8, 9, 22}) {  // Audio, Video, Aggregate
        CHECK(resolve_delivery(type, DeliveryMode::Auto, true, 100, kMax,
                               DatagramFallback::Stream) ==
              ResolvedMode::Datagram);
    }
}

TEST_CASE("Auto falls back to stream when not negotiated or too large") {
    CHECK(resolve_delivery(9, DeliveryMode::Auto, false, 100, kMax,
                           DatagramFallback::Stream) == ResolvedMode::Stream);
    CHECK(resolve_delivery(9, DeliveryMode::Auto, true, kMax + 1, kMax,
                           DatagramFallback::Stream) == ResolvedMode::Stream);
}

TEST_CASE("Datagram mode honors the fallback policy") {
    CHECK(resolve_delivery(9, DeliveryMode::Datagram, true, 100, kMax,
                           DatagramFallback::Stream) ==
          ResolvedMode::Datagram);
    CHECK(resolve_delivery(9, DeliveryMode::Datagram, true, kMax + 1, kMax,
                           DatagramFallback::Stream) == ResolvedMode::Stream);
    CHECK(resolve_delivery(9, DeliveryMode::Datagram, true, kMax + 1, kMax,
                           DatagramFallback::Drop) == ResolvedMode::Dropped);
    CHECK(resolve_delivery(9, DeliveryMode::Datagram, false, 100, kMax,
                           DatagramFallback::Drop) == ResolvedMode::Dropped);
    CHECK(resolve_delivery(9, DeliveryMode::Datagram, false, 100, kMax,
                           DatagramFallback::Stream) == ResolvedMode::Stream);
}
```

`tests/quic/outbound_queue_test.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <thread>

#include "roqr/quic/outbound_queue.hpp"

using namespace roqr::quic;

namespace {
roqr::Frame make_frame(uint64_t ts) {
    roqr::Frame f;
    f.message_type = 9;
    f.timestamp = ts;
    f.payload = {0x01};
    return f;
}
}  // namespace

TEST_CASE("queue is FIFO") {
    OutboundQueue q;
    q.push({make_frame(1), DeliveryMode::Stream});
    q.push({make_frame(2), DeliveryMode::Datagram});
    auto a = q.pop();
    auto b = q.pop();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->frame.timestamp == 1);
    CHECK(a->mode == DeliveryMode::Stream);
    CHECK(b->frame.timestamp == 2);
    CHECK_FALSE(q.pop().has_value());
}

TEST_CASE("concurrent producers do not lose items") {
    OutboundQueue q;
    constexpr int kPerThread = 500;
    std::thread t1([&] {
        for (int i = 0; i < kPerThread; ++i)
            q.push({make_frame(1), DeliveryMode::Stream});
    });
    std::thread t2([&] {
        for (int i = 0; i < kPerThread; ++i)
            q.push({make_frame(2), DeliveryMode::Stream});
    });
    t1.join();
    t2.join();
    CHECK(q.size() == 2 * kPerThread);
}
```

Add both to `tests/CMakeLists.txt` inside the `if(ROQR_BUILD_QUIC)` block:

```cmake
  add_executable(roqr-quic-tests
    quic/context_test.cpp
    quic/delivery_test.cpp
    quic/outbound_queue_test.cpp
  )
```

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to compile (missing headers `roqr/quic/delivery.hpp`, `roqr/quic/outbound_queue.hpp`)

- [ ] **Step 3: Implement**

`quic/include/roqr/quic/delivery.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace roqr::quic {

enum class DeliveryMode { Stream, Datagram, Auto };

// What to do when a Datagram-mode frame cannot go in a datagram
// (extension not negotiated, or frame exceeds the max datagram size):
// send it on the stream instead (default) or drop it (draft s7.5).
enum class DatagramFallback { Stream, Drop };

enum class ResolvedMode { Stream, Datagram, Dropped };

// Applies draft s7.5/s10 policy. Auto keeps session-correctness traffic
// (commands, control, data/metadata, shared objects) on streams; audio (8),
// video (9), and aggregate (22) messages use datagrams when negotiated and
// the encoded frame fits.
ResolvedMode resolve_delivery(uint8_t message_type, DeliveryMode requested,
                              bool datagram_negotiated, size_t encoded_size,
                              size_t max_datagram_size,
                              DatagramFallback fallback);

}  // namespace roqr::quic
```

`quic/src/delivery.cpp`:

```cpp
#include "roqr/quic/delivery.hpp"

namespace roqr::quic {

namespace {
bool is_media_type(uint8_t message_type) {
    return message_type == 8 || message_type == 9 || message_type == 22;
}
}  // namespace

ResolvedMode resolve_delivery(uint8_t message_type, DeliveryMode requested,
                              bool datagram_negotiated, size_t encoded_size,
                              size_t max_datagram_size,
                              DatagramFallback fallback) {
    if (requested == DeliveryMode::Stream) return ResolvedMode::Stream;

    const bool fits = datagram_negotiated && encoded_size <= max_datagram_size;

    if (requested == DeliveryMode::Datagram) {
        if (fits) return ResolvedMode::Datagram;
        return fallback == DatagramFallback::Stream ? ResolvedMode::Stream
                                                    : ResolvedMode::Dropped;
    }

    // Auto: media only, and only when it fits; everything else on stream.
    if (is_media_type(message_type) && fits) return ResolvedMode::Datagram;
    return ResolvedMode::Stream;
}

}  // namespace roqr::quic
```

`quic/include/roqr/quic/outbound_queue.hpp`:

```cpp
#pragma once

#include <deque>
#include <mutex>
#include <optional>

#include "roqr/frame.hpp"
#include "roqr/quic/delivery.hpp"

namespace roqr::quic {

struct Outbound {
    roqr::Frame frame;
    DeliveryMode mode;
};

// Thread-safe FIFO between app threads (push) and the network thread (pop).
class OutboundQueue {
public:
    void push(Outbound item) {
        std::lock_guard lock(mutex_);
        items_.push_back(std::move(item));
    }

    std::optional<Outbound> pop() {
        std::lock_guard lock(mutex_);
        if (items_.empty()) return std::nullopt;
        Outbound item = std::move(items_.front());
        items_.pop_front();
        return item;
    }

    size_t size() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

private:
    mutable std::mutex mutex_;
    std::deque<Outbound> items_;
};

}  // namespace roqr::quic
```

Add `src/delivery.cpp` to `quic/CMakeLists.txt`:

```cmake
add_library(roqr-quic STATIC
  src/context.cpp
  src/delivery.cpp
)
```

- [ ] **Step 4: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 37 prior + 7 new = 44 test cases.

- [ ] **Step 5: Commit**

```bash
git add quic tests
git commit -m "Add delivery-mode policy and outbound queue"
```

---

### Task 3: relayd server library, echo mode, cert fixture

**Files:**
- Create: `tools/relayd/include/roqr/relayd/server.hpp`
- Create: `tools/relayd/src/server.cpp`
- Create: `tools/relayd/src/main.cpp`
- Create: `tools/relayd/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt` (cert fixture + link relayd lib + integration test file)
- Test: `tests/integration/server_lifecycle_test.cpp`

**Interfaces:**
- Consumes: `QuicContext` (Task 1), `roqr::Frame`, `frame_encode`, `FrameDecoder`, `datagram_decode` from core.
- Produces:
  - `enum class roqr::relayd::Mode { Echo, Relay }`
  - `struct roqr::relayd::ServerOptions { uint16_t port; std::string cert_file; std::string key_file; Mode mode = Mode::Echo; std::string alpn = "roqr"; }`
  - `class roqr::relayd::Server`: `bool start(const ServerOptions&)` (spawns the network thread; false on failure), `void stop()` (idempotent, joins), destructor calls `stop()`.
  - Echo behavior: every complete RoQR frame received on any stream is re-encoded and sent back on the same stream; every datagram frame is echoed as a datagram. Relay behavior arrives in Task 8 (start() with Mode::Relay behaves as Echo until then — documented in the header).
  - Test cert fixture: CMake generates `cert.pem`/`key.pem` into `${CMAKE_CURRENT_BINARY_DIR}/testcerts/` at build time; tests read the path from the compile definition `ROQR_TEST_CERT_DIR`.

- [ ] **Step 1: Write the failing test `tests/integration/server_lifecycle_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "roqr/relayd/server.hpp"

using namespace roqr::relayd;

namespace {
const std::string kCertDir = ROQR_TEST_CERT_DIR;
}

TEST_CASE("server starts on a loopback port and stops cleanly") {
    Server server;
    ServerOptions opts;
    opts.port = 45550;
    opts.cert_file = kCertDir + "/cert.pem";
    opts.key_file = kCertDir + "/key.pem";
    REQUIRE(server.start(opts));
    server.stop();
    // stop() is idempotent.
    server.stop();
}

TEST_CASE("server start fails with missing certs") {
    Server server;
    ServerOptions opts;
    opts.port = 45551;
    opts.cert_file = "/nonexistent/cert.pem";
    opts.key_file = "/nonexistent/key.pem";
    CHECK_FALSE(server.start(opts));
}
```

- [ ] **Step 2: Add cert fixture and test wiring to `tests/CMakeLists.txt`** (inside the `if(ROQR_BUILD_QUIC)` block; requires `ROQR_BUILD_TOOLS`):

```cmake
  if(ROQR_BUILD_TOOLS)
    set(ROQR_TEST_CERT_DIR "${CMAKE_CURRENT_BINARY_DIR}/testcerts")
    add_custom_command(
      OUTPUT "${ROQR_TEST_CERT_DIR}/cert.pem" "${ROQR_TEST_CERT_DIR}/key.pem"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${ROQR_TEST_CERT_DIR}"
      COMMAND openssl req -x509 -newkey ec
              -pkeyopt ec_paramgen_curve:prime256v1
              -keyout "${ROQR_TEST_CERT_DIR}/key.pem"
              -out "${ROQR_TEST_CERT_DIR}/cert.pem"
              -days 365 -nodes -subj "/CN=localhost"
      VERBATIM)
    add_custom_target(roqr-testcerts DEPENDS
      "${ROQR_TEST_CERT_DIR}/cert.pem" "${ROQR_TEST_CERT_DIR}/key.pem")

    add_executable(roqr-integration-tests
      integration/server_lifecycle_test.cpp
    )
    add_dependencies(roqr-integration-tests roqr-testcerts)
    target_compile_definitions(roqr-integration-tests PRIVATE
      ROQR_TEST_CERT_DIR="${ROQR_TEST_CERT_DIR}")
    target_link_libraries(roqr-integration-tests PRIVATE
      roqr-relayd-lib roqr-quic Catch2::Catch2WithMain)
    catch_discover_tests(roqr-integration-tests PROPERTIES TIMEOUT 60)
  endif()
```

- [ ] **Step 3: Run to verify RED**

Run: `cmake --preset dev 2>&1 | tail -3`
Expected: FAIL to configure (no target `roqr-relayd-lib`) or compile failure on missing `roqr/relayd/server.hpp`.

- [ ] **Step 4: Implement the server**

`tools/relayd/include/roqr/relayd/server.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace roqr::relayd {

// Echo: reflect every RoQR frame back to its sender on the same carriage
// (stream frames on the same stream, datagram frames as datagrams).
// Relay: forward frames to all other connections (Task 8; until then Relay
// behaves as Echo).
enum class Mode { Echo, Relay };

struct ServerOptions {
    uint16_t port = 0;
    std::string cert_file;
    std::string key_file;
    Mode mode = Mode::Echo;
    std::string alpn = "roqr";
};

class Server {
public:
    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(const ServerOptions& options);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace roqr::relayd
```

`tools/relayd/src/server.cpp`:

```cpp
#include "roqr/relayd/server.hpp"

#include <sys/stat.h>

#include <map>
#include <mutex>
#include <vector>

#include <picoquic.h>
#include <picoquic_packet_loop.h>
#include <picoquic_utils.h>

#include "roqr/frame.hpp"
#include "roqr/quic/context.hpp"

namespace roqr::relayd {

namespace {
bool file_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}
}  // namespace

struct Server::Impl {
    ServerOptions options;
    std::unique_ptr<roqr::quic::QuicContext> quic;
    picoquic_network_thread_ctx_t* thread_ctx = nullptr;
    int thread_ret = 0;
    std::mutex mutex;
    bool running = false;

    // Per-connection state, touched only on the network thread.
    struct Conn {
        std::map<uint64_t, roqr::FrameDecoder> decoders;  // by stream id
    };
    std::map<picoquic_cnx_t*, Conn> conns;

    static int connection_callback(picoquic_cnx_t* cnx, uint64_t stream_id,
                                   uint8_t* bytes, size_t length,
                                   picoquic_call_back_event_t event,
                                   void* callback_ctx, void* stream_ctx);
    static int loop_callback(picoquic_quic_t* quic,
                             picoquic_packet_loop_cb_enum cb_mode,
                             void* callback_ctx, void* callback_arg);

    void echo_stream_frames(picoquic_cnx_t* cnx, uint64_t stream_id,
                            const uint8_t* bytes, size_t length);
};

void Server::Impl::echo_stream_frames(picoquic_cnx_t* cnx,
                                      uint64_t stream_id,
                                      const uint8_t* bytes, size_t length) {
    auto& decoder = conns[cnx].decoders.try_emplace(stream_id).first->second;
    decoder.feed(std::span<const uint8_t>(bytes, length));
    while (auto frame = decoder.next()) {
        std::vector<uint8_t> wire;
        if (roqr::frame_encode(*frame, wire)) {
            picoquic_add_to_stream(cnx, stream_id, wire.data(), wire.size(),
                                   0);
        }
    }
}

int Server::Impl::connection_callback(picoquic_cnx_t* cnx,
                                      uint64_t stream_id, uint8_t* bytes,
                                      size_t length,
                                      picoquic_call_back_event_t event,
                                      void* callback_ctx,
                                      void* /*stream_ctx*/) {
    auto* impl = static_cast<Impl*>(callback_ctx);
    switch (event) {
        case picoquic_callback_stream_data:
        case picoquic_callback_stream_fin:
            impl->echo_stream_frames(cnx, stream_id, bytes, length);
            break;
        case picoquic_callback_datagram: {
            roqr::Frame frame;
            if (roqr::datagram_decode(std::span<const uint8_t>(bytes, length),
                                      frame) == roqr::DecodeStatus::Ok) {
                std::vector<uint8_t> wire;
                if (roqr::frame_encode(frame, wire)) {
                    picoquic_queue_datagram_frame(cnx, wire.size(),
                                                  wire.data());
                }
            }
            // Malformed datagrams are dropped without closing (draft s12).
            break;
        }
        case picoquic_callback_close:
        case picoquic_callback_application_close:
            impl->conns.erase(cnx);
            break;
        default:
            break;
    }
    return 0;
}

int Server::Impl::loop_callback(picoquic_quic_t* /*quic*/,
                                picoquic_packet_loop_cb_enum /*cb_mode*/,
                                void* /*callback_ctx*/,
                                void* /*callback_arg*/) {
    return 0;
}

Server::Server() : impl_(std::make_unique<Impl>()) {}
Server::~Server() { stop(); }

bool Server::start(const ServerOptions& options) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->running) return false;
    if (!file_exists(options.cert_file) || !file_exists(options.key_file)) {
        return false;
    }
    impl_->options = options;
    impl_->quic = roqr::quic::QuicContext::create_server(
        options.alpn, options.cert_file, options.key_file,
        &Impl::connection_callback, impl_.get());
    if (!impl_->quic) return false;

    picoquic_packet_loop_param_t param{};
    param.local_port = options.port;
    param.local_af = AF_INET;
    impl_->thread_ctx = picoquic_start_network_thread(
        impl_->quic->get(), &param, &Impl::loop_callback, impl_.get(),
        &impl_->thread_ret);
    if (impl_->thread_ctx == nullptr) {
        impl_->quic.reset();
        return false;
    }
    impl_->running = true;
    return true;
}

void Server::stop() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->running) return;
    picoquic_delete_network_thread(impl_->thread_ctx);
    impl_->thread_ctx = nullptr;
    impl_->quic.reset();
    impl_->conns.clear();
    impl_->running = false;
}

}  // namespace roqr::relayd
```

`tools/relayd/src/main.cpp`:

```cpp
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <semaphore>

#include "roqr/relayd/server.hpp"

namespace {
std::binary_semaphore g_stop{0};
void handle_signal(int) { g_stop.release(); }
}  // namespace

int main(int argc, char** argv) {
    roqr::relayd::ServerOptions opts;
    opts.port = 4443;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            opts.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--cert") == 0 && i + 1 < argc) {
            opts.cert_file = argv[++i];
        } else if (std::strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            opts.key_file = argv[++i];
        } else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            ++i;
            opts.mode = std::strcmp(argv[i], "relay") == 0
                            ? roqr::relayd::Mode::Relay
                            : roqr::relayd::Mode::Echo;
        } else {
            std::fprintf(stderr,
                         "usage: roqr-relayd --cert C --key K [--port P] "
                         "[--mode echo|relay]\n");
            return 2;
        }
    }
    if (opts.cert_file.empty() || opts.key_file.empty()) {
        std::fprintf(stderr, "error: --cert and --key are required\n");
        return 2;
    }

    roqr::relayd::Server server;
    if (!server.start(opts)) {
        std::fprintf(stderr, "error: failed to start server\n");
        return 1;
    }
    std::printf("roqr-relayd listening on port %u\n", opts.port);
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    g_stop.acquire();
    server.stop();
    return 0;
}
```

`tools/relayd/CMakeLists.txt`:

```cmake
add_library(roqr-relayd-lib STATIC
  src/server.cpp
)

target_include_directories(roqr-relayd-lib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(roqr-relayd-lib PUBLIC roqr-quic)

target_compile_options(roqr-relayd-lib PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)

add_executable(roqr-relayd src/main.cpp)
target_link_libraries(roqr-relayd PRIVATE roqr-relayd-lib)
```

- [ ] **Step 5: Run to verify GREEN**

Run: `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 44 prior + 2 new = 46. If `picoquic_start_network_thread` or `picoquic_packet_loop_param_t` fields differ on the pinned commit, apply the API-drift rule.

- [ ] **Step 6: Commit**

```bash
git add tools tests
git commit -m "Add relayd echo server with lifecycle test and cert fixture"
```

---

### Task 4: Client connect/close lifecycle

**Files:**
- Create: `quic/include/roqr/quic/client.hpp`
- Create: `quic/src/client.cpp`
- Modify: `quic/CMakeLists.txt` (add `src/client.cpp`)
- Modify: `tests/CMakeLists.txt` (add `integration/client_connect_test.cpp`)
- Test: `tests/integration/client_connect_test.cpp`

**Interfaces:**
- Consumes: `QuicContext`, `OutboundQueue`, `DeliveryMode`, `resolve_delivery` (Tasks 1-2); `Frame`, `FrameDecoder`, `datagram_decode`, `frame_encode`, `FlowTable`, `ErrorCode` from core; `roqr::relayd::Server` (Task 3, tests only).
- Produces (full public surface; later tasks fill in the send/receive internals):

```cpp
namespace roqr::quic {
struct ClientOptions {
    std::string alpn = "roqr";
    bool insecure_skip_verify = true;
    DatagramFallback datagram_fallback = DatagramFallback::Stream;
    roqr::FlowTableLimits flow_limits{};
};
class Client {
public:
    using MessageHandler = std::function<void(const roqr::Frame&)>;
    using ClosedHandler = std::function<void(uint64_t app_error_code)>;
    Client();
    ~Client();  // stops the network thread
    void on_message(MessageHandler h);   // set before connect()
    void on_closed(ClosedHandler h);     // set before connect()
    bool connect(const std::string& host, uint16_t port,
                 ClientOptions options = {});
    bool wait_connected(std::chrono::milliseconds timeout);
    bool send(roqr::Frame frame, DeliveryMode mode);  // thread-safe
    void bind_flow(uint64_t flow_id);
    void retire_flow(uint64_t flow_id);
    void reset_flow_stream(uint64_t flow_id);         // Task 9
    void close(roqr::ErrorCode code = roqr::ErrorCode::NoError);
    bool wait_closed(std::chrono::milliseconds timeout);
};
}
```

  - This task implements: construction, `connect` (context + `picoquic_create_client_cnx` + `picoquic_start_network_thread`), readiness signaling (`picoquic_callback_ready` -> condition variable -> `wait_connected`), `close` (queued to the network thread, executed as `picoquic_close`), `wait_closed`, destructor teardown (stop thread, free). `send`/`bind_flow`/`retire_flow`/`reset_flow_stream` exist but `send` returns false and the flow calls only touch the FlowTable — wire-up comes in Tasks 5-7 and 9.

- [ ] **Step 1: Write the failing test `tests/integration/client_connect_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "roqr/quic/client.hpp"
#include "roqr/relayd/server.hpp"

using namespace std::chrono_literals;

namespace {
const std::string kCertDir = ROQR_TEST_CERT_DIR;

roqr::relayd::ServerOptions server_opts(uint16_t port) {
    roqr::relayd::ServerOptions o;
    o.port = port;
    o.cert_file = kCertDir + "/cert.pem";
    o.key_file = kCertDir + "/key.pem";
    return o;
}
}  // namespace

TEST_CASE("client completes the roqr handshake against the relay") {
    roqr::relayd::Server server;
    REQUIRE(server.start(server_opts(45552)));

    roqr::quic::Client client;
    REQUIRE(client.connect("127.0.0.1", 45552));
    CHECK(client.wait_connected(5s));

    client.close();
    CHECK(client.wait_closed(5s));
    server.stop();
}

TEST_CASE("wait_connected times out when no server is listening") {
    roqr::quic::Client client;
    REQUIRE(client.connect("127.0.0.1", 45553));  // nothing listening
    CHECK_FALSE(client.wait_connected(2s));
}
```

Add to the integration test target sources in `tests/CMakeLists.txt`:

```cmake
    add_executable(roqr-integration-tests
      integration/server_lifecycle_test.cpp
      integration/client_connect_test.cpp
    )
```

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev 2>&1 | tail -5`
Expected: FAIL to compile (missing `roqr/quic/client.hpp`)

- [ ] **Step 3: Implement `quic/include/roqr/quic/client.hpp`**

```cpp
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "roqr/error.hpp"
#include "roqr/flow_table.hpp"
#include "roqr/frame.hpp"
#include "roqr/quic/delivery.hpp"

namespace roqr::quic {

struct ClientOptions {
    std::string alpn = "roqr";
    bool insecure_skip_verify = true;
    DatagramFallback datagram_fallback = DatagramFallback::Stream;
    roqr::FlowTableLimits flow_limits{};
};

// RoQR client over picoquic. One background network thread owns all
// picoquic calls; handlers fire on that thread and must not block.
// send()/close()/bind_flow()/retire_flow() are safe from any thread.
class Client {
public:
    using MessageHandler = std::function<void(const roqr::Frame&)>;
    using ClosedHandler = std::function<void(uint64_t app_error_code)>;

    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Set handlers before connect(); they run on the network thread.
    void on_message(MessageHandler h);
    void on_closed(ClosedHandler h);

    bool connect(const std::string& host, uint16_t port,
                 ClientOptions options = {});
    bool wait_connected(std::chrono::milliseconds timeout);

    bool send(roqr::Frame frame, DeliveryMode mode);
    void bind_flow(uint64_t flow_id);
    void retire_flow(uint64_t flow_id);
    void reset_flow_stream(uint64_t flow_id);

    void close(roqr::ErrorCode code = roqr::ErrorCode::NoError);
    bool wait_closed(std::chrono::milliseconds timeout);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace roqr::quic
```

- [ ] **Step 4: Implement `quic/src/client.cpp`**

```cpp
#include "roqr/quic/client.hpp"

#include <condition_variable>
#include <map>
#include <mutex>
#include <vector>

#include <picoquic.h>
#include <picoquic_packet_loop.h>
#include <picoquic_utils.h>

#include "roqr/quic/context.hpp"
#include "roqr/quic/outbound_queue.hpp"

namespace roqr::quic {

struct Client::Impl {
    ClientOptions options;
    std::unique_ptr<QuicContext> quic;
    picoquic_cnx_t* cnx = nullptr;  // network thread only after connect
    picoquic_network_thread_ctx_t* thread_ctx = nullptr;
    int thread_ret = 0;

    MessageHandler message_handler;
    ClosedHandler closed_handler;

    std::mutex mutex;
    std::condition_variable cv;
    bool connected = false;
    bool closed = false;
    uint64_t close_code = 0;

    // App -> network thread.
    OutboundQueue outbound;
    bool close_requested = false;
    uint64_t requested_close_code = 0;

    // Network-thread-only state (Tasks 5-7 fill these in).
    roqr::FlowTable flows;
    std::map<uint64_t, roqr::FrameDecoder> decoders;  // by stream id

    void wake() {
        if (thread_ctx != nullptr) {
            picoquic_wake_up_network_thread(thread_ctx);
        }
    }

    void signal_connected() {
        std::lock_guard lock(mutex);
        connected = true;
        cv.notify_all();
    }

    void signal_closed(uint64_t code) {
        {
            std::lock_guard lock(mutex);
            if (closed) return;
            closed = true;
            close_code = code;
            cv.notify_all();
        }
        if (closed_handler) closed_handler(code);
    }

    // Runs on the network thread: apply app-thread requests.
    void service();

    static int connection_callback(picoquic_cnx_t* cnx, uint64_t stream_id,
                                   uint8_t* bytes, size_t length,
                                   picoquic_call_back_event_t event,
                                   void* callback_ctx, void* stream_ctx);
    static int loop_callback(picoquic_quic_t* quic,
                             picoquic_packet_loop_cb_enum cb_mode,
                             void* callback_ctx, void* callback_arg);
};

void Client::Impl::service() {
    bool do_close = false;
    uint64_t code = 0;
    {
        std::lock_guard lock(mutex);
        do_close = close_requested;
        code = requested_close_code;
        close_requested = false;
    }
    if (do_close && cnx != nullptr) {
        picoquic_close(cnx, code);
    }
    // Tasks 5-6 drain `outbound` here.
}

int Client::Impl::connection_callback(picoquic_cnx_t* /*cnx*/,
                                      uint64_t /*stream_id*/,
                                      uint8_t* /*bytes*/, size_t /*length*/,
                                      picoquic_call_back_event_t event,
                                      void* callback_ctx,
                                      void* /*stream_ctx*/) {
    auto* impl = static_cast<Impl*>(callback_ctx);
    switch (event) {
        case picoquic_callback_ready:
            impl->signal_connected();
            break;
        case picoquic_callback_close:
            impl->signal_closed(0);
            break;
        case picoquic_callback_application_close:
            impl->signal_closed(picoquic_get_application_error(impl->cnx));
            break;
        default:
            break;  // stream/datagram events arrive in Tasks 5-6
    }
    return 0;
}

int Client::Impl::loop_callback(picoquic_quic_t* /*quic*/,
                                picoquic_packet_loop_cb_enum cb_mode,
                                void* callback_ctx, void* /*callback_arg*/) {
    auto* impl = static_cast<Impl*>(callback_ctx);
    switch (cb_mode) {
        case picoquic_packet_loop_wake_up:
        case picoquic_packet_loop_after_receive:
        case picoquic_packet_loop_after_send:
            impl->service();
            break;
        default:
            break;
    }
    return 0;
}

Client::Client() : impl_(std::make_unique<Impl>()) {}

Client::~Client() {
    if (impl_->thread_ctx != nullptr) {
        picoquic_delete_network_thread(impl_->thread_ctx);
        impl_->thread_ctx = nullptr;
    }
}

void Client::on_message(MessageHandler h) {
    impl_->message_handler = std::move(h);
}
void Client::on_closed(ClosedHandler h) {
    impl_->closed_handler = std::move(h);
}

bool Client::connect(const std::string& host, uint16_t port,
                     ClientOptions options) {
    impl_->options = std::move(options);
    impl_->flows = roqr::FlowTable(impl_->options.flow_limits);
    impl_->quic = QuicContext::create_client(
        impl_->options.alpn, impl_->options.insecure_skip_verify);
    if (!impl_->quic) return false;

    struct sockaddr_storage addr {};
    int is_name = 0;
    if (picoquic_get_server_address(host.c_str(), port, &addr, &is_name) !=
        0) {
        return false;
    }

    const uint64_t now = picoquic_current_time();
    impl_->cnx = picoquic_create_client_cnx(
        impl_->quic->get(), reinterpret_cast<struct sockaddr*>(&addr), now,
        0, host.c_str(), impl_->options.alpn.c_str(),
        &Impl::connection_callback, impl_.get());
    if (impl_->cnx == nullptr) return false;

    picoquic_packet_loop_param_t param{};
    param.local_af = AF_INET;
    impl_->thread_ctx = picoquic_start_network_thread(
        impl_->quic->get(), &param, &Impl::loop_callback, impl_.get(),
        &impl_->thread_ret);
    return impl_->thread_ctx != nullptr;
}

bool Client::wait_connected(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->cv.wait_for(lock, timeout,
                              [&] { return impl_->connected; });
}

bool Client::send(roqr::Frame /*frame*/, DeliveryMode /*mode*/) {
    return false;  // Task 5 implements the stream path, Task 6 datagrams.
}

void Client::bind_flow(uint64_t flow_id) {
    std::lock_guard lock(impl_->mutex);
    impl_->flows.activate(flow_id);  // full wiring in Task 7
}

void Client::retire_flow(uint64_t flow_id) {
    std::lock_guard lock(impl_->mutex);
    impl_->flows.retire(flow_id);
}

void Client::reset_flow_stream(uint64_t /*flow_id*/) {
    // Task 9.
}

void Client::close(roqr::ErrorCode code) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->close_requested = true;
        impl_->requested_close_code = static_cast<uint64_t>(code);
    }
    impl_->wake();
}

bool Client::wait_closed(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->cv.wait_for(lock, timeout, [&] { return impl_->closed; });
}

}  // namespace roqr::quic
```

Add `src/client.cpp` to `quic/CMakeLists.txt`:

```cmake
add_library(roqr-quic STATIC
  src/client.cpp
  src/context.cpp
  src/delivery.cpp
)
```

- [ ] **Step 5: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 46 prior + 2 new = 48. The no-server case must time out (bounded) and not hang: `wait_connected(2s)` returns false. Note: a `picoquic_callback_close` before ready signals a failed handshake; the test does not assert on that path beyond the timeout.

- [ ] **Step 6: Commit**

```bash
git add quic tests
git commit -m "Add client connect and close lifecycle over loopback"
```

---

### Task 5: Stream send and receive path

**Files:**
- Modify: `quic/src/client.cpp` (implement send + stream receive)
- Test: `tests/integration/stream_echo_test.cpp` (new; add to tests/CMakeLists.txt)

**Interfaces:**
- Consumes: everything from Task 4; relayd Echo behavior from Task 3.
- Produces: working `Client::send(frame, DeliveryMode::Stream)` — encodes the frame and queues it; the network thread drains the queue with `picoquic_add_to_stream` on the stream bound to the frame's flow (flow 0 -> the first client bidi stream, id 0; other flows get their own bidi stream via `picoquic_get_next_local_stream_id(cnx, 0)` on first use). Incoming stream data feeds a per-stream `FrameDecoder`; complete frames are delivered to `on_message` (flow handling refined in Task 7 — for now every received frame is delivered directly). Malformed stream framing closes the connection with `FRAME_ENCODING_ERROR` (draft s12).

- [ ] **Step 1: Write the failing test `tests/integration/stream_echo_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "roqr/quic/client.hpp"
#include "roqr/relayd/server.hpp"

using namespace std::chrono_literals;

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
    bool wait_count(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return frames.size() >= n; });
    }
};

roqr::Frame video_frame(uint64_t ts, std::vector<uint8_t> payload) {
    roqr::Frame f;
    f.flow_id = 0;
    f.timestamp = ts;
    f.message_type = 9;
    f.message_stream_id = 1;
    f.chunk_stream_id = 4;
    f.payload = std::move(payload);
    return f;
}
}  // namespace

TEST_CASE("stream frames echo back in order") {
    roqr::relayd::Server server;
    roqr::relayd::ServerOptions so;
    so.port = 45554;
    so.cert_file = kCertDir + "/cert.pem";
    so.key_file = kCertDir + "/key.pem";
    REQUIRE(server.start(so));

    Collector got;
    roqr::quic::Client client;
    client.on_message([&](const roqr::Frame& f) { got.add(f); });
    REQUIRE(client.connect("127.0.0.1", 45554));
    REQUIRE(client.wait_connected(5s));

    const auto a = video_frame(100, {0xAA, 0xBB});
    const auto b = video_frame(200, std::vector<uint8_t>(3000, 0xCC));
    REQUIRE(client.send(a, roqr::quic::DeliveryMode::Stream));
    REQUIRE(client.send(b, roqr::quic::DeliveryMode::Stream));

    REQUIRE(got.wait_count(2, 5s));
    {
        std::lock_guard lock(got.mutex);
        CHECK(got.frames[0] == a);
        CHECK(got.frames[1] == b);
    }
    client.close();
    client.wait_closed(5s);
    server.stop();
}

TEST_CASE("send fails before connect") {
    roqr::quic::Client client;
    CHECK_FALSE(client.send(video_frame(1, {0x01}),
                            roqr::quic::DeliveryMode::Stream));
}
```

(The 3000-byte payload forces the echoed frame to span multiple QUIC packets, exercising incremental reassembly.)

Add `integration/stream_echo_test.cpp` to the `roqr-integration-tests` sources.

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev && ctest --preset dev 2>&1 | tail -5`
Expected: new test FAILS (send returns false).

- [ ] **Step 3: Implement in `quic/src/client.cpp`**

Add to `Impl` (state + helper):

```cpp
    // Network-thread-only: flow id -> bidi stream id for sending.
    std::map<uint64_t, uint64_t> flow_streams;
    bool started = false;  // set once connect() succeeds

    uint64_t stream_for_flow(uint64_t flow_id) {
        auto it = flow_streams.find(flow_id);
        if (it != flow_streams.end()) return it->second;
        uint64_t id;
        if (flow_id == 0) {
            id = 0;  // first client-initiated bidi stream
        } else {
            id = picoquic_get_next_local_stream_id(cnx, 0);
        }
        flow_streams[flow_id] = id;
        return id;
    }

    void deliver(const roqr::Frame& frame) {
        if (message_handler) message_handler(frame);
    }

    void fail_connection(roqr::ErrorCode code) {
        if (cnx != nullptr) {
            picoquic_close(cnx, static_cast<uint64_t>(code));
        }
    }

    void on_stream_data(uint64_t stream_id, const uint8_t* bytes,
                        size_t length) {
        auto& decoder = decoders.try_emplace(stream_id).first->second;
        decoder.feed(std::span<const uint8_t>(bytes, length));
        while (auto frame = decoder.next()) {
            deliver(*frame);  // Task 7 adds FlowTable gating here
        }
        if (decoder.malformed()) {
            fail_connection(roqr::ErrorCode::FrameEncodingError);
        }
    }
```

Replace the `service()` body's send section (keep the close handling):

```cpp
void Client::Impl::service() {
    bool do_close = false;
    uint64_t code = 0;
    {
        std::lock_guard lock(mutex);
        do_close = close_requested;
        code = requested_close_code;
        close_requested = false;
    }
    if (do_close && cnx != nullptr) {
        picoquic_close(cnx, code);
        return;
    }
    while (auto item = outbound.pop()) {
        std::vector<uint8_t> wire;
        if (!roqr::frame_encode(item->frame, wire)) continue;
        // Task 6 consults resolve_delivery here; for now everything is
        // stream-carried.
        const uint64_t stream_id = stream_for_flow(item->frame.flow_id);
        picoquic_add_to_stream(cnx, stream_id, wire.data(), wire.size(), 0);
    }
}
```

Replace `connection_callback`'s default case with stream handling:

```cpp
        case picoquic_callback_stream_data:
        case picoquic_callback_stream_fin:
            impl->on_stream_data(stream_id, bytes, length);
            break;
```

(and un-void the `stream_id`/`bytes`/`length` parameters).

Replace `Client::send`:

```cpp
bool Client::send(roqr::Frame frame, DeliveryMode mode) {
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->started || impl_->closed) return false;
    }
    if (frame.payload.empty()) return false;
    impl_->outbound.push({std::move(frame), mode});
    impl_->wake();
    return true;
}
```

In `Client::connect`, set `impl_->started = true;` (under the mutex) just before returning `impl_->thread_ctx != nullptr` — only when the thread actually started:

```cpp
    if (impl_->thread_ctx == nullptr) return false;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->started = true;
    }
    return true;
```

- [ ] **Step 4: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 48 prior + 2 new = 50.

- [ ] **Step 5: Commit**

```bash
git add quic tests
git commit -m "Add stream send and receive path with echo round-trip test"
```

---

### Task 6: Datagram path and Auto mode

**Files:**
- Modify: `quic/src/client.cpp` (datagram TX/RX, resolve_delivery wiring)
- Test: `tests/integration/datagram_test.cpp` (new; add to tests/CMakeLists.txt)

**Interfaces:**
- Consumes: Task 5's send/receive plumbing; `resolve_delivery` from Task 2.
- Produces: `send` consults `resolve_delivery(message_type, mode, datagram_negotiated, wire.size(), max_datagram, fallback)`; Datagram-resolved frames go out via `picoquic_queue_datagram_frame`, Dropped frames are counted and skipped. Incoming `picoquic_callback_datagram` events run `datagram_decode`; `Ok` frames deliver, `Malformed` datagrams are dropped silently (draft s12). Datagram negotiation: the client sets `max_datagram_frame_size` in its transport parameters before connecting; negotiation state is read from picoquic at ready time.

- [ ] **Step 1: Write the failing test `tests/integration/datagram_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "roqr/quic/client.hpp"
#include "roqr/relayd/server.hpp"

using namespace std::chrono_literals;

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
    bool wait_count(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return frames.size() >= n; });
    }
};

roqr::Frame media(uint64_t ts, size_t payload_size) {
    roqr::Frame f;
    f.flow_id = 0;
    f.timestamp = ts;
    f.message_type = 9;
    f.message_stream_id = 1;
    f.chunk_stream_id = 4;
    f.payload.assign(payload_size, 0xEE);
    return f;
}
}  // namespace

TEST_CASE("small media frame round-trips as a datagram") {
    roqr::relayd::Server server;
    roqr::relayd::ServerOptions so;
    so.port = 45555;
    so.cert_file = kCertDir + "/cert.pem";
    so.key_file = kCertDir + "/key.pem";
    REQUIRE(server.start(so));

    Collector got;
    roqr::quic::Client client;
    client.on_message([&](const roqr::Frame& f) { got.add(f); });
    REQUIRE(client.connect("127.0.0.1", 45555));
    REQUIRE(client.wait_connected(5s));

    const auto f = media(300, 100);
    REQUIRE(client.send(f, roqr::quic::DeliveryMode::Datagram));
    REQUIRE(got.wait_count(1, 5s));
    {
        std::lock_guard lock(got.mutex);
        CHECK(got.frames[0] == f);
    }
    client.close();
    client.wait_closed(5s);
    server.stop();
}

TEST_CASE("oversized datagram falls back to stream and still arrives") {
    roqr::relayd::Server server;
    roqr::relayd::ServerOptions so;
    so.port = 45556;
    so.cert_file = kCertDir + "/cert.pem";
    so.key_file = kCertDir + "/key.pem";
    REQUIRE(server.start(so));

    Collector got;
    roqr::quic::Client client;
    client.on_message([&](const roqr::Frame& f) { got.add(f); });
    REQUIRE(client.connect("127.0.0.1", 45556));
    REQUIRE(client.wait_connected(5s));

    // Far larger than any datagram: must arrive via the stream fallback.
    const auto f = media(400, 10000);
    REQUIRE(client.send(f, roqr::quic::DeliveryMode::Datagram));
    REQUIRE(got.wait_count(1, 5s));
    {
        std::lock_guard lock(got.mutex);
        CHECK(got.frames[0] == f);
    }
    client.close();
    client.wait_closed(5s);
    server.stop();
}
```

Add `integration/datagram_test.cpp` to the `roqr-integration-tests` sources.

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev && ctest --preset dev 2>&1 | tail -5`
Expected: datagram test FAILS (frames currently always stream-carried is fine for arrival, but the first test asserts datagram carriage indirectly only via arrival — to make RED meaningful the test must fail: it will, because datagram negotiation is not yet enabled and `picoquic_queue_datagram_frame` is not called; the frame still arrives via stream. So instead RED is verified by the *negotiation* assertions below.)

**Correction — make the first test RED-able:** add this stronger assertion to the first test after `wait_connected` (include it in Step 1 when writing the file):

```cpp
    CHECK(client.datagrams_negotiated());
```

and add to the public API in `quic/include/roqr/quic/client.hpp` (declare in Step 3):

```cpp
    // True once the connection is ready and the peer accepted the QUIC
    // DATAGRAM extension (draft s4).
    bool datagrams_negotiated() const;
```

With that, RED = `datagrams_negotiated` does not exist -> compile failure.

- [ ] **Step 3: Implement in `quic/src/client.cpp`**

Enable the extension before creating the connection (in `Client::connect`, right after `create_client` succeeds):

```cpp
    // Advertise DATAGRAM support (RFC 9221): nonzero max_datagram_frame_size.
    picoquic_tp_t tp;
    picoquic_init_transport_parameters(&tp, 1 /* client */);
    tp.max_datagram_frame_size = 1536;
    picoquic_set_default_tp(impl_->quic->get(), &tp);
```

(API-drift rule applies: if the pinned picoquic names these differently — e.g. a dedicated setter — adapt and record it.)

Add to `Impl`:

```cpp
    bool datagrams_ok = false;   // set at ready time, network thread
    size_t max_datagram = 0;
    uint64_t dropped_datagrams = 0;
```

In `connection_callback`, on `picoquic_callback_ready`, query negotiation before signaling:

```cpp
        case picoquic_callback_ready: {
            const picoquic_tp_t* remote_tp =
                picoquic_get_transport_parameters(impl->cnx, 0 /* remote */);
            if (remote_tp != nullptr &&
                remote_tp->max_datagram_frame_size > 0) {
                impl->datagrams_ok = true;
                impl->max_datagram =
                    static_cast<size_t>(remote_tp->max_datagram_frame_size);
            }
            impl->signal_connected();
            break;
        }
```

(API-drift rule: the remote-TP accessor may be `picoquic_get_transport_parameters(cnx, int client_mode)` or a direct struct read; adapt to the pinned header and record.)

Add the datagram receive case:

```cpp
        case picoquic_callback_datagram: {
            roqr::Frame frame;
            if (roqr::datagram_decode(std::span<const uint8_t>(bytes, length),
                                      frame) == roqr::DecodeStatus::Ok) {
                impl->deliver(frame);
            }
            break;  // malformed datagrams dropped silently (draft s12)
        }
```

Wire `resolve_delivery` into `service()`'s drain loop (replacing the always-stream line):

```cpp
    while (auto item = outbound.pop()) {
        std::vector<uint8_t> wire;
        if (!roqr::frame_encode(item->frame, wire)) continue;
        const ResolvedMode resolved = resolve_delivery(
            item->frame.message_type, item->mode, datagrams_ok, wire.size(),
            max_datagram, options.datagram_fallback);
        switch (resolved) {
            case ResolvedMode::Datagram:
                if (picoquic_queue_datagram_frame(cnx, wire.size(),
                                                  wire.data()) != 0) {
                    ++dropped_datagrams;  // dropped-not-blocked semantics
                }
                break;
            case ResolvedMode::Stream: {
                const uint64_t stream_id =
                    stream_for_flow(item->frame.flow_id);
                picoquic_add_to_stream(cnx, stream_id, wire.data(),
                                       wire.size(), 0);
                break;
            }
            case ResolvedMode::Dropped:
                ++dropped_datagrams;
                break;
        }
    }
```

Add the accessor (header declaration shown in Step 2's correction):

```cpp
bool Client::datagrams_negotiated() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->connected && impl_->datagrams_ok;
}
```

(`datagrams_ok` is written on the network thread before `signal_connected` takes the mutex, so reading it under the same mutex after `connected` is true is safe.)

Also include `"roqr/quic/delivery.hpp"` usage is already present via client.hpp.

- [ ] **Step 4: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 50 prior + 2 new = 52. If `datagrams_negotiated()` is false against the echo server, the server side also needs the TP set: apply the same `picoquic_set_default_tp` block in `Server::start` after `create_server` (server mode: `picoquic_init_transport_parameters(&tp, 0)`) — do this proactively in Step 3 if the first run fails on negotiation, and note it.

- [ ] **Step 5: Commit**

```bash
git add quic tests
git commit -m "Add datagram path with negotiation and auto fallback"
```

---

### Task 7: Flow gating on receive

**Files:**
- Modify: `quic/src/client.cpp` (FlowTable gating in deliver path, drain on bind)
- Test: `tests/integration/flow_gating_test.cpp` (new; add to tests/CMakeLists.txt)

**Interfaces:**
- Consumes: Tasks 4-6 internals; `FlowTable` semantics from core (Plan 1).
- Produces: received frames whose flow is `Active` deliver immediately; `Unknown` flows buffer within `FlowTableLimits` (stream overflow -> connection close with `UNKNOWN_FLOW_ID` — the client is not obligated to keep the connection; draft s5 allows closing — datagram overflow -> drop); `bind_flow(flow_id)` drains buffered frames to `on_message` in arrival order (executed on the network thread via a queued request, keeping FlowTable network-thread-only after connect); `Retired` flows drop incoming frames.

- [ ] **Step 1: Write the failing test `tests/integration/flow_gating_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "roqr/quic/client.hpp"
#include "roqr/relayd/server.hpp"

using namespace std::chrono_literals;

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
    bool wait_count(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return frames.size() >= n; });
    }
    size_t count() {
        std::lock_guard lock(mutex);
        return frames.size();
    }
};

roqr::Frame flow_frame(uint64_t flow_id, uint64_t ts) {
    roqr::Frame f;
    f.flow_id = flow_id;
    f.timestamp = ts;
    f.message_type = 9;
    f.message_stream_id = 1;
    f.chunk_stream_id = 4;
    f.payload = {0x0F};
    return f;
}
}  // namespace

TEST_CASE("frames for an unbound flow buffer until bind_flow") {
    roqr::relayd::Server server;
    roqr::relayd::ServerOptions so;
    so.port = 45557;
    so.cert_file = kCertDir + "/cert.pem";
    so.key_file = kCertDir + "/key.pem";
    REQUIRE(server.start(so));

    Collector got;
    roqr::quic::Client client;
    client.on_message([&](const roqr::Frame& f) { got.add(f); });
    REQUIRE(client.connect("127.0.0.1", 45557));
    REQUIRE(client.wait_connected(5s));

    // Echoed back with flow 7, which this client never bound for receive.
    REQUIRE(client.send(flow_frame(7, 1), roqr::quic::DeliveryMode::Stream));
    // Flow 0 frame proves the echo round-trip completed while flow 7 waits.
    REQUIRE(client.send(flow_frame(0, 2), roqr::quic::DeliveryMode::Stream));
    REQUIRE(got.wait_count(1, 5s));
    CHECK(got.frames[0].flow_id == 0);
    CHECK(got.count() == 1);  // flow 7 buffered, not delivered

    client.bind_flow(7);
    REQUIRE(got.wait_count(2, 5s));
    {
        std::lock_guard lock(got.mutex);
        CHECK(got.frames[1].flow_id == 7);
        CHECK(got.frames[1].timestamp == 1);
    }
    client.close();
    client.wait_closed(5s);
    server.stop();
}

TEST_CASE("frames for a retired flow are dropped") {
    roqr::relayd::Server server;
    roqr::relayd::ServerOptions so;
    so.port = 45558;
    so.cert_file = kCertDir + "/cert.pem";
    so.key_file = kCertDir + "/key.pem";
    REQUIRE(server.start(so));

    Collector got;
    roqr::quic::Client client;
    client.on_message([&](const roqr::Frame& f) { got.add(f); });
    REQUIRE(client.connect("127.0.0.1", 45558));
    REQUIRE(client.wait_connected(5s));

    client.bind_flow(9);
    client.retire_flow(9);
    REQUIRE(client.send(flow_frame(9, 1), roqr::quic::DeliveryMode::Stream));
    REQUIRE(client.send(flow_frame(0, 2), roqr::quic::DeliveryMode::Stream));
    REQUIRE(got.wait_count(1, 5s));
    // Only the flow 0 frame arrives; the retired flow 9 echo was dropped.
    CHECK(got.frames[0].flow_id == 0);
    CHECK(got.count() == 1);

    client.close();
    client.wait_closed(5s);
    server.stop();
}
```

Add `integration/flow_gating_test.cpp` to the `roqr-integration-tests` sources.

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev && ctest --preset dev 2>&1 | tail -5`
Expected: first test FAILS — flow 7 frame is delivered immediately (count reaches 2 before bind), because Task 5 delivers unconditionally.

- [ ] **Step 3: Implement in `quic/src/client.cpp`**

`bind_flow`/`retire_flow` become queued requests so FlowTable stays network-thread-only after connect. Add to `Impl`:

```cpp
    // App -> network thread flow requests, drained in service().
    std::vector<std::pair<uint64_t, bool>> flow_requests;  // (id, activate)
```

Replace `Client::bind_flow` / `Client::retire_flow`:

```cpp
void Client::bind_flow(uint64_t flow_id) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->flow_requests.emplace_back(flow_id, true);
    }
    impl_->wake();
}

void Client::retire_flow(uint64_t flow_id) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->flow_requests.emplace_back(flow_id, false);
    }
    impl_->wake();
}
```

In `service()`, before draining `outbound`:

```cpp
    std::vector<std::pair<uint64_t, bool>> requests;
    {
        std::lock_guard lock(mutex);
        requests.swap(flow_requests);
    }
    for (auto [flow_id, activate] : requests) {
        if (activate) {
            if (flows.activate(flow_id)) {
                for (auto& frame : flows.take_buffered(flow_id)) {
                    deliver(frame);
                }
            }
        } else {
            flows.retire(flow_id);
        }
    }
```

Replace the delivery inside `on_stream_data` (the `while (auto frame = ...)` body) with gated delivery:

```cpp
        while (auto frame = decoder.next()) {
            gate_and_deliver(std::move(*frame), /*from_stream=*/true);
        }
```

and the datagram receive case's deliver with:

```cpp
                impl->gate_and_deliver(std::move(frame),
                                       /*from_stream=*/false);
```

Add the gate to `Impl`:

```cpp
    void gate_and_deliver(roqr::Frame frame, bool from_stream) {
        switch (flows.state(frame.flow_id)) {
            case roqr::FlowState::Active:
                deliver(frame);
                break;
            case roqr::FlowState::Retired:
                break;  // dropped
            case roqr::FlowState::Unknown:
                if (flows.buffer_unknown(std::move(frame)) ==
                    roqr::FlowTable::BufferResult::LimitExceeded) {
                    if (from_stream) {
                        // Bounded buffering exhausted (draft s5).
                        fail_connection(roqr::ErrorCode::UnknownFlowId);
                    }
                    // Datagram overflow: drop silently.
                }
                break;
        }
    }
```

Also remove the direct `flows.activate/retire` calls that Task 4 put in `bind_flow`/`retire_flow` (they are replaced above), and delete the now-unused mutex lock pattern there.

- [ ] **Step 4: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 52 prior + 2 new = 54.

- [ ] **Step 5: Commit**

```bash
git add quic tests
git commit -m "Gate received frames through the flow table"
```

---

### Task 8: Relay mode and two-client integration

**Files:**
- Modify: `tools/relayd/src/server.cpp` (Relay mode: forward to other connections)
- Test: `tests/integration/relay_test.cpp` (new; add to tests/CMakeLists.txt)

**Interfaces:**
- Consumes: Server Echo internals (Task 3), Client (Tasks 4-7).
- Produces: with `Mode::Relay`, a frame received from connection A is forwarded to every *other* live connection (same Flow ID, stream frames on the receiving stream id opened toward each peer — the relay uses one bidi stream per (source stream id) mapping; simplest correct form: forward stream frames on the same stream id it received them on, toward each other connection, and datagram frames as datagrams). Echo mode behavior unchanged.

- [ ] **Step 1: Write the failing test `tests/integration/relay_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "roqr/quic/client.hpp"
#include "roqr/relayd/server.hpp"

using namespace std::chrono_literals;

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
    bool wait_count(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return frames.size() >= n; });
    }
};
}  // namespace

TEST_CASE("relay forwards frames from publisher to subscriber") {
    roqr::relayd::Server server;
    roqr::relayd::ServerOptions so;
    so.port = 45559;
    so.cert_file = kCertDir + "/cert.pem";
    so.key_file = kCertDir + "/key.pem";
    so.mode = roqr::relayd::Mode::Relay;
    REQUIRE(server.start(so));

    Collector pub_got, sub_got;
    roqr::quic::Client subscriber;
    subscriber.on_message([&](const roqr::Frame& f) { sub_got.add(f); });
    REQUIRE(subscriber.connect("127.0.0.1", 45559));
    REQUIRE(subscriber.wait_connected(5s));

    roqr::quic::Client publisher;
    publisher.on_message([&](const roqr::Frame& f) { pub_got.add(f); });
    REQUIRE(publisher.connect("127.0.0.1", 45559));
    REQUIRE(publisher.wait_connected(5s));

    roqr::Frame f;
    f.flow_id = 0;
    f.timestamp = 42;
    f.message_type = 9;
    f.message_stream_id = 1;
    f.chunk_stream_id = 4;
    f.payload = {0xDE, 0xAD, 0xBE, 0xEF};
    REQUIRE(publisher.send(f, roqr::quic::DeliveryMode::Stream));

    REQUIRE(sub_got.wait_count(1, 5s));
    {
        std::lock_guard lock(sub_got.mutex);
        CHECK(sub_got.frames[0] == f);
    }
    // Relay must not echo back to the sender.
    CHECK_FALSE(pub_got.wait_count(1, 1s));

    publisher.close();
    subscriber.close();
    publisher.wait_closed(5s);
    subscriber.wait_closed(5s);
    server.stop();
}
```

Add `integration/relay_test.cpp` to the `roqr-integration-tests` sources.

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev && ctest --preset dev 2>&1 | tail -5`
Expected: relay test FAILS — Relay currently behaves as Echo (subscriber gets nothing; publisher gets its own frame back).

- [ ] **Step 3: Implement in `tools/relayd/src/server.cpp`**

In `Impl::echo_stream_frames` and the datagram case, branch on mode. Replace the two forwarding sites with calls to a common helper, and add connection tracking:

```cpp
    void forward_frame(picoquic_cnx_t* from, uint64_t stream_id,
                       const roqr::Frame& frame, bool as_datagram) {
        std::vector<uint8_t> wire;
        if (!roqr::frame_encode(frame, wire)) return;
        if (options.mode == Mode::Echo) {
            if (as_datagram) {
                picoquic_queue_datagram_frame(from, wire.size(), wire.data());
            } else {
                picoquic_add_to_stream(from, stream_id, wire.data(),
                                       wire.size(), 0);
            }
            return;
        }
        // Relay: forward to every other live connection.
        for (auto& [cnx, conn] : conns) {
            if (cnx == from) continue;
            if (as_datagram) {
                picoquic_queue_datagram_frame(cnx, wire.size(), wire.data());
            } else {
                picoquic_add_to_stream(cnx, stream_id, wire.data(),
                                       wire.size(), 0);
            }
        }
    }
```

Rewrite `echo_stream_frames` (rename to `handle_stream_frames`) to use it:

```cpp
void Server::Impl::handle_stream_frames(picoquic_cnx_t* cnx,
                                        uint64_t stream_id,
                                        const uint8_t* bytes, size_t length) {
    auto& decoder = conns[cnx].decoders.try_emplace(stream_id).first->second;
    decoder.feed(std::span<const uint8_t>(bytes, length));
    while (auto frame = decoder.next()) {
        forward_frame(cnx, stream_id, *frame, /*as_datagram=*/false);
    }
}
```

And the datagram case:

```cpp
        case picoquic_callback_datagram: {
            roqr::Frame frame;
            if (roqr::datagram_decode(std::span<const uint8_t>(bytes, length),
                                      frame) == roqr::DecodeStatus::Ok) {
                impl->conns.try_emplace(cnx);  // track datagram-only conns
                impl->forward_frame(cnx, 0, frame, /*as_datagram=*/true);
            }
            break;
        }
```

Also ensure new connections are tracked on first stream data (the `conns[cnx]` in `handle_stream_frames` already inserts). Update the header comment in `tools/relayd/include/roqr/relayd/server.hpp` to remove the "Relay behaves as Echo" note:

```cpp
// Echo: reflect every RoQR frame back to its sender on the same carriage.
// Relay: forward every RoQR frame to all other live connections, preserving
// Flow ID, stream id, and carriage; the sender does not get it back.
```

- [ ] **Step 4: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 54 prior + 1 new = 55.

- [ ] **Step 5: Commit**

```bash
git add tools tests
git commit -m "Add relay mode forwarding frames between connections"
```

---

### Task 9: Flow stream reset and close-code propagation

**Files:**
- Modify: `quic/src/client.cpp` (`reset_flow_stream`, close-code plumbing checks)
- Test: `tests/integration/close_codes_test.cpp` (new; add to tests/CMakeLists.txt)

**Interfaces:**
- Consumes: everything prior.
- Produces: `Client::reset_flow_stream(flow_id)` queues a network-thread request that calls `picoquic_reset_stream(cnx, stream_id, FRAME_CANCELLED)` for the flow's bound send stream (no-op if the flow has no stream yet) — draft s8/s11 stale-media cancellation; `Client::close(code)` propagates the given Table 2 code to the peer, verified end-to-end via a second client observing... (the echo server does not surface peer close codes; instead the test verifies the *local* `on_closed` code from a server-initiated path is 0/NoError, and reset does not kill the connection).

- [ ] **Step 1: Write the failing test `tests/integration/close_codes_test.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "roqr/quic/client.hpp"
#include "roqr/relayd/server.hpp"

using namespace std::chrono_literals;

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
    bool wait_count(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return frames.size() >= n; });
    }
};

roqr::Frame flow_frame(uint64_t flow_id, uint64_t ts) {
    roqr::Frame f;
    f.flow_id = flow_id;
    f.timestamp = ts;
    f.message_type = 9;
    f.message_stream_id = 1;
    f.chunk_stream_id = 4;
    f.payload = {0x0F};
    return f;
}
}  // namespace

TEST_CASE("reset_flow_stream cancels a flow stream without killing the connection") {
    roqr::relayd::Server server;
    roqr::relayd::ServerOptions so;
    so.port = 45560;
    so.cert_file = kCertDir + "/cert.pem";
    so.key_file = kCertDir + "/key.pem";
    REQUIRE(server.start(so));

    Collector got;
    roqr::quic::Client client;
    client.on_message([&](const roqr::Frame& f) { got.add(f); });
    REQUIRE(client.connect("127.0.0.1", 45560));
    REQUIRE(client.wait_connected(5s));

    // Establish a send stream for flow 3, then cancel it.
    client.bind_flow(3);
    REQUIRE(client.send(flow_frame(3, 1), roqr::quic::DeliveryMode::Stream));
    REQUIRE(got.wait_count(1, 5s));
    client.reset_flow_stream(3);

    // Connection survives: flow 0 traffic still round-trips afterwards.
    REQUIRE(client.send(flow_frame(0, 2), roqr::quic::DeliveryMode::Stream));
    REQUIRE(got.wait_count(2, 5s));

    client.close(roqr::ErrorCode::NoError);
    CHECK(client.wait_closed(5s));
    server.stop();
}

TEST_CASE("reset_flow_stream on a flow without a stream is a safe no-op") {
    roqr::quic::Client client;
    client.reset_flow_stream(99);  // must not crash before connect
}
```

Add `integration/close_codes_test.cpp` to the `roqr-integration-tests` sources.

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build --preset dev && ctest --preset dev 2>&1 | tail -5`
Expected: first test FAILS at the post-reset round-trip only if reset misbehaves; the guaranteed RED is that `reset_flow_stream` is currently an empty stub, so add this assertion instead — after `client.reset_flow_stream(3);` the *echo of a further flow-3 send must not arrive* (the send stream is gone and Task 5's `stream_for_flow` must allocate a fresh stream on next use — asserting the reset actually reached picoquic):

Include in Step 1's first test, right after `client.reset_flow_stream(3);`:

```cpp
    // A further flow 3 send must still work: the client allocates a fresh
    // stream after reset (reset only cancels the old stream).
    REQUIRE(client.send(flow_frame(3, 9), roqr::quic::DeliveryMode::Stream));
    REQUIRE(got.wait_count(2, 5s));
```

(then the flow 0 round-trip below becomes wait_count(3, 5s)). RED: with the stub, the flow-3 re-send reuses the same healthy stream and the test still passes — so the *only* reliable RED assertion for the stub is stream-id observability, which the API does not expose. Accept a weaker RED: `reset_flow_stream` stub means the "fresh stream" comment is untestable; the test still fails RED because the stub returns without queueing and the *no-op safety* second test passes trivially. If the first test passes against the stub, note that in the report and rely on the implementation review — do NOT delete assertions.

- [ ] **Step 3: Implement in `quic/src/client.cpp`**

Add to `Impl`:

```cpp
    std::vector<uint64_t> reset_requests;  // app -> network thread
```

Replace `Client::reset_flow_stream`:

```cpp
void Client::reset_flow_stream(uint64_t flow_id) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->reset_requests.push_back(flow_id);
    }
    impl_->wake();
}
```

In `service()`, after the flow requests block:

```cpp
    std::vector<uint64_t> resets;
    {
        std::lock_guard lock(mutex);
        resets.swap(reset_requests);
    }
    for (uint64_t flow_id : resets) {
        auto it = flow_streams.find(flow_id);
        if (it == flow_streams.end()) continue;
        picoquic_reset_stream(
            cnx, it->second,
            static_cast<uint64_t>(roqr::ErrorCode::FrameCancelled));
        flow_streams.erase(it);  // next send opens a fresh stream
    }
```

Guard `wake()` for the pre-connect no-op case (already null-checked via `thread_ctx`).

Note: for flow 0, `stream_for_flow` hardcodes stream 0; after a reset of flow 0 the fresh stream must NOT reuse id 0. Change `stream_for_flow` so the flow-0 special case applies only when no stream was ever assigned:

```cpp
    uint64_t stream_for_flow(uint64_t flow_id) {
        auto it = flow_streams.find(flow_id);
        if (it != flow_streams.end()) return it->second;
        uint64_t id;
        if (flow_id == 0 && !flow0_stream_used) {
            id = 0;
            flow0_stream_used = true;
        } else {
            id = picoquic_get_next_local_stream_id(cnx, 0);
        }
        flow_streams[flow_id] = id;
        return id;
    }
```

with `bool flow0_stream_used = false;` added to `Impl`. (`picoquic_get_next_local_stream_id(cnx, 0)` returns the lowest unused client bidi id, which is 0 on first call — if so the special case is redundant and you may simplify to always use `picoquic_get_next_local_stream_id`; verify against the pinned header and record which form you used.)

- [ ] **Step 4: Run to verify GREEN**

Run: `cmake --build --preset dev && ctest --preset dev`
Expected: PASS — 55 prior + 2 new = 57. Also re-run the full suite twice to shake out port-reuse flakiness: `ctest --preset dev --repeat until-fail:2`.

- [ ] **Step 5: Commit**

```bash
git add quic tests
git commit -m "Add flow stream reset and fresh-stream reallocation"
```

---

## Completion Criteria

- `ctest --preset dev` green (57 cases: 35 core + 9 quic unit + 13 integration), warning-clean build, `--repeat until-fail:2` stable.
- `roqr-relayd` binary runs standalone (`--cert/--key/--port/--mode`).
- Draft coverage added: s4 ALPN `roqr` + DATAGRAM negotiation, s5 flow gating with bounded buffering over a real connection, s7.4/s7.5 carriage, s8/s11 FRAME_CANCELLED stream reset, s10 Auto policy, s12 malformed handling (stream close vs datagram drop), Table 2 codes on the wire via `picoquic_close`.
- Client API surface ready for Plan 5 FFI wrapping.

## Follow-On Plans

1. Plan 3: `roqr-rtmp` handshake, chunking, AMF0, E-RTMP-aware media classifier.
2. Plan 4: gateway examples, relayd AMF0 command handling, ffmpeg e2e incl. E-RTMP HEVC case, GitHub Actions CI.
3. Plan 5: C FFI, JNI bindings, Java samples.
