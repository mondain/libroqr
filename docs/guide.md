# libroqr Guide: Development, Deployment, and Configuration

libroqr is a C++20 implementation of RoQR (RTMP over QUIC,
draft-gregoire-rtmp-over-quic): a sans-I/O protocol core, a picoquic
transport, an RTMP/AMF gateway, a reference relay, and C/JNI bindings.

The reference media path is:

```
ffmpeg (RTMP publish) -> roqr-ingest -> roqr-relayd -> roqr-egress -> ffmpeg (RTMP play)
```

RoQR carries RTMP message metadata and payloads over QUIC streams and DATAGRAM
frames. The gateways let existing RTMP tooling publish and play across a QUIC
network without changes.

This guide has three parts:

- [Part 1: Developer guide](#part-1-developer-guide) — building, testing, sanitizers, CI, layout.
- [Part 2: Deployment](#part-2-deployment) — running the relay and gateways, TLS, an end-to-end example.
- [Part 3: Configuration reference](#part-3-configuration-reference) — every build option, environment variable, CLI flag, and library option.

---

## Part 1: Developer guide

### Prerequisites

| Tool | Version | Needed for |
|------|---------|------------|
| C++ compiler | GCC 11+ or Clang 14+ (C++20) | everything |
| CMake | 3.24+ | everything |
| OpenSSL | 1.1+/3.x with headers (`libssl-dev`) | the QUIC transport (picotls) |
| Git | any recent | fetching pinned picoquic/picotls |
| ffmpeg | any recent | the end-to-end tests and the reference path |
| A JDK | 17+ | JNI bindings (`ROQR_BUILD_JNI=ON`) only |
| Ninja | optional | faster builds |

### Getting the QUIC dependencies

The QUIC transport links pinned versions of picoquic and picotls. A helper
script clones and builds them under `.deps/` and prints the environment
variables the build needs:

```
eval "$(scripts/setup_picoquic_deps.sh)"
```

This exports `ROQR_PICOQUIC_SOURCE_DIR` and `ROQR_PICOTLS_PREFIX`. Run it once
per shell before configuring or building anything that needs QUIC. The pinned
revisions are picoquic `55b473e2` and picotls `7c32032f`.

picotls is built with position-independent code so its static libraries can
link into the shared `libroqr-ffi.so`; picoquic gets PIC from the project's
own CMake. The core protocol library needs neither dependency.

### Building

The simplest path uses the CMake presets:

```
cmake --preset dev       # configure: QUIC + tools + tests + JNI, into build/dev
cmake --build --preset dev
ctest --preset dev
```

The `dev` preset enables `ROQR_BUILD_QUIC`, `ROQR_BUILD_TOOLS`,
`ROQR_BUILD_JNI`, and `ROQR_BUILD_TESTS` (examples and FFI are on by default).
If no JDK is present the JNI targets are skipped with a warning; everything
else still builds.

The core protocol library builds with no QUIC dependency at all:

```
cmake -S . -B build -DROQR_BUILD_QUIC=OFF -DROQR_BUILD_EXAMPLES=OFF
cmake --build build
```

Note: a bare `cmake -S . -B build` with all defaults does **not** configure,
because `ROQR_BUILD_EXAMPLES` defaults on but requires `ROQR_BUILD_QUIC` and
`ROQR_BUILD_RTMP`. Use a preset, or set the options explicitly (see the
[build options table](#build-time-options-cmake)).

### Running the tests

```
ctest --preset dev --output-on-failure
```

Integration and relay tests need a TLS certificate/key pair. CMake generates a
throwaway self-signed pair automatically (the `roqr-testcerts` target) using
OpenSSL, so no manual setup is required. The two ffmpeg end-to-end tests
(`roqr-ffmpeg-e2e-h264`, `-hevc`) spawn real ffmpeg processes and need ffmpeg
on `PATH`.

### Sanitizers and ThreadSanitizer

The `ROQR_SANITIZE` option builds ROQR's own targets under a sanitizer
(`thread`, `address`, or `undefined`):

```
eval "$(scripts/setup_picoquic_deps.sh)"
CC=clang CXX=clang++ cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug \
  -DROQR_SANITIZE=thread \
  -DROQR_BUILD_QUIC=ON -DROQR_BUILD_TOOLS=ON -DROQR_BUILD_EXAMPLES=ON \
  -DROQR_BUILD_TESTS=ON -DROQR_BUILD_JNI=OFF
cmake --build build/tsan --parallel
TSAN_OPTIONS="suppressions=$PWD/cmake/tsan.supp" \
  ctest --test-dir build/tsan --output-on-failure --timeout 300
```

Only ROQR code is instrumented; the vendored picoquic is not. picoquic has
internal network-thread and PRNG data races that libroqr cannot fix and that
would otherwise drown out races in our code. Because our `on_message` /
`on_closed` callbacks are our own (instrumented) code even though they run on
picoquic's network thread, TSAN still detects real races in ROQR while leaving
picoquic's internals untracked.

`cmake/tsan.supp` narrowly suppresses only picoquic's wake-up-pipe file
descriptor races (which TSAN's syscall interceptors flag regardless of
instrumentation). It deliberately does **not** suppress
`picoquic_packet_loop_poll`, since that frame appears in the stacks of our
network-thread callbacks — suppressing it could hide a real callback race.

### Continuous integration

`.github/workflows/ci.yml` runs on every push to `main` and every pull request:

| Job | What it does |
|-----|--------------|
| `core-only (no picoquic)` | Builds and tests just the sans-I/O core (`ROQR_BUILD_QUIC=OFF`). |
| `full (gcc)` / `full (clang)` | Full build with QUIC, tools, examples; runs the whole suite including the ffmpeg e2e tests. |
| `thread-sanitizer (clang)` | Full build with `ROQR_SANITIZE=thread`; runs the suite under TSAN with the suppressions file. A race in ROQR code fails the job. |

### Project layout

- `core/` — sans-I/O RoQR frame codec and flow table (no dependencies).
- `quic/` — picoquic client transport (`roqr::quic::Client`).
- `rtmp/` — RTMP handshake, chunking, AMF0, E-RTMP media classifier, server session/listener.
- `gateway/` — RTMP <-> RoQR bridge, ingest/egress gateways, connection supervisor.
- `tools/relayd/` — the RoQR reference relay (`roqr-relayd`).
- `examples/` — `roqr-ingest`, `roqr-egress`, `roqr-duplex`, and Java samples under `examples/java/`.
- `ffi/` — C ABI (`roqr.h`, `roqr_rtmp.h`) in `libroqr-ffi.so`.
- `jni/` — JNI bindings (`org.red5.roqr`) in `libroqr-jni.so` + `roqr.jar`.
- `tests/` — unit and integration tests.
- `cmake/` — `FindPicoquic.cmake`, `tsan.supp`, `android-jni.md`.

### Conventions

Builds are warning-clean under `-Wall -Wextra`; keep them that way. New
concurrency should be exercised under the TSAN job. Native callbacks
(`on_message`, `on_closed`, JNI `MessageListener`) run on a network thread and
must not block or throw.

---

## Part 2: Deployment

### Components

| Binary | Role | Default RTMP port | Default RoQR (QUIC) |
|--------|------|-------------------|---------------------|
| `roqr-relayd` | RoQR reference relay/server | — | listens on `:4443` |
| `roqr-ingest` | RTMP publisher -> RoQR | listens on `:1935` | connects to relay |
| `roqr-egress` | RoQR -> RTMP player | listens on `:1936` | connects to relay |
| `roqr-duplex` | ingest + egress in one process | ingest `:1935`, egress `:1936` | connects to relay |

The gateways are one-publisher / one-player each ("gateway-grade"). The relay
routes between many connections.

### TLS certificates

The QUIC transport requires TLS. The relay needs a certificate and private key
(PEM). For local or test use, generate a self-signed pair:

```
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
  -keyout key.pem -out cert.pem -days 365 -nodes -subj "/CN=localhost"
```

Clients verify the server certificate unless `insecure_skip_verify` is set. The
example gateways and the library default `insecure_skip_verify` to `true`,
which is appropriate for local testing but **must not** be used against an
untrusted network — see [Security notes](#security-notes).

### Running the relay

```
roqr-relayd --cert cert.pem --key key.pem [--port 4443] [--mode echo|relay|media]
```

`--cert` and `--key` are required. Modes:

- `echo` (default) — reflect every RoQR frame back to its sender.
- `relay` — forward every frame to all other connections (Flow ID and carriage preserved; stream ids are not).
- `media` — parse RTMP commands, register publishers/subscribers by stream name, replay cached init frames to late subscribers, and route media from publisher to subscribers. This is the mode used for the reference RTMP path.

### Running the gateways

```
roqr-ingest [--rtmp-port 1935] [--roqr-host 127.0.0.1] [--roqr-port 4443]
roqr-egress [--rtmp-port 1936] [--roqr-host 127.0.0.1] [--roqr-port 4443] [--stream cam]
roqr-duplex [--roqr-host 127.0.0.1] [--roqr-port 4443] [--stream cam]
```

The gateways connect to the relay lazily and **auto-reconnect** if the relay
connection drops: a supervisor thread rebuilds the QUIC connection with
exponential backoff and replays the RTMP session handshake (publish for
ingest, play for egress). After a bounded number of consecutive failures it
gives up (see [ReconnectPolicy](#roqrgatewayreconnectpolicy)).

### End-to-end example

Terminal 1 — the relay in media mode:

```
roqr-relayd --cert cert.pem --key key.pem --port 4443 --mode media
```

Terminal 2 — egress serving stream `cam` to an RTMP player:

```
roqr-egress --rtmp-port 1936 --roqr-host 127.0.0.1 --roqr-port 4443 --stream cam
ffplay rtmp://127.0.0.1:1936/live/cam
```

Terminal 3 — ingest bridging an RTMP publisher into the relay:

```
roqr-ingest --rtmp-port 1935 --roqr-host 127.0.0.1 --roqr-port 4443
ffmpeg -re -i input.mp4 -c copy -f flv rtmp://127.0.0.1:1935/live/cam
```

`roqr-duplex` collapses ingest and egress into a single process against one
relay, which is convenient for a local loopback test.

### Security notes

libroqr is not yet hardened for untrusted, internet-facing input. Before
exposing it beyond a trusted network:

- Set `insecure_skip_verify = false` and provide proper certificate
  verification. The default of `true` skips server certificate validation.
- Note that RTMP chunk/AMF parsing has not been fuzzed yet; treat the RTMP
  listener as trusted-input only for now.

---

## Part 3: Configuration reference

### Build-time options (CMake)

All are `-D<name>=<value>` at configure time.

| Option | Default | Meaning |
|--------|---------|---------|
| `ROQR_BUILD_TESTS` | `ON` | Build the unit and integration tests. |
| `ROQR_BUILD_QUIC` | `OFF` | Build the picoquic transport (needs picoquic/picotls). |
| `ROQR_BUILD_RTMP` | `ON` | Build the RTMP/AMF gateway module. |
| `ROQR_BUILD_TOOLS` | `OFF` | Build `roqr-relayd` (requires QUIC + RTMP). |
| `ROQR_BUILD_EXAMPLES` | `ON` | Build the example gateways (requires QUIC + RTMP; with tests also requires TOOLS). |
| `ROQR_BUILD_FFI` | `ON` | Build `libroqr-ffi.so` (needs QUIC + RTMP; skipped otherwise). |
| `ROQR_BUILD_JNI` | `OFF` | Build JNI bindings + `roqr.jar` (needs the FFI library and a JDK, or Android). |
| `ROQR_SANITIZE` | `""` | Sanitize ROQR targets: `thread`, `address`, or `undefined` (empty = none). |
| `CMAKE_BUILD_TYPE` | (unset) | `Debug` / `Release` etc. The `dev` preset uses Debug, `release` uses Release. |

Because `ROQR_BUILD_EXAMPLES` defaults on and requires QUIC + RTMP, configure
via a preset or set the options explicitly rather than relying on bare
defaults.

### Environment variables

| Variable | Set by | Used for |
|----------|--------|----------|
| `ROQR_PICOQUIC_SOURCE_DIR` | `scripts/setup_picoquic_deps.sh` | Locating the picoquic source tree at configure time. |
| `ROQR_PICOTLS_PREFIX` | `scripts/setup_picoquic_deps.sh` | Locating the built picotls libraries. |
| `TSAN_OPTIONS` | you, for TSAN runs | e.g. `suppressions=$PWD/cmake/tsan.supp`. |

### Runtime CLI flags

**`roqr-relayd`**

| Flag | Default | Meaning |
|------|---------|---------|
| `--cert <file>` | (required) | Server certificate (PEM). |
| `--key <file>` | (required) | Server private key (PEM). |
| `--port <n>` | `4443` | UDP/QUIC listen port. |
| `--mode <echo\|relay\|media>` | `echo` | Relay behavior (see [Running the relay](#running-the-relay)). |

**`roqr-ingest`**

| Flag | Default | Meaning |
|------|---------|---------|
| `--rtmp-port <n>` | `1935` | RTMP listen port for the publisher. |
| `--roqr-host <h>` | `127.0.0.1` | Relay host. |
| `--roqr-port <n>` | `4443` | Relay QUIC port. |

**`roqr-egress`**

| Flag | Default | Meaning |
|------|---------|---------|
| `--rtmp-port <n>` | `1936` | RTMP listen port for the player. |
| `--roqr-host <h>` | `127.0.0.1` | Relay host. |
| `--roqr-port <n>` | `4443` | Relay QUIC port. |
| `--stream <name>` | `cam` | Stream name to play from the relay. |

**`roqr-duplex`**

| Flag | Default | Meaning |
|------|---------|---------|
| `--roqr-host <h>` | `127.0.0.1` | Relay host (shared). |
| `--roqr-port <n>` | `4443` | Relay QUIC port (shared). |
| `--stream <name>` | `cam` | Egress stream name. Ingest RTMP is `:1935`, egress RTMP is `:1936`. |

### Library options

These structs configure the library directly (and back the CLI flags and the
FFI/JNI surface).

#### `roqr::quic::ClientOptions`

| Field | Default | Meaning |
|-------|---------|---------|
| `alpn` | `"roqr"` | QUIC ALPN protocol identifier. |
| `insecure_skip_verify` | `true` | Skip server certificate verification. Testing only. |
| `datagram_fallback` | `Stream` | What to do when a Datagram-mode frame cannot use a datagram: `Stream` (send on a stream) or `Drop`. |
| `flow_limits` | see below | Buffering limits for frames on not-yet-bound flows. |
| `idle_timeout` | `15000` ms | Max time to wait for peer activity before declaring the connection dead. `0` keeps picoquic's 30s default. Keepalive runs at half this interval so idle-but-alive connections survive. This bound is what lets the gateways notice a silently-dropped relay. |
| `handshake_timeout` | `10000` ms | Max time to wait for the QUIC handshake before a connect attempt is counted as failed. `0` keeps picoquic's default. Keep it well below `idle_timeout`. |

#### `roqr::FlowTableLimits` (`ClientOptions::flow_limits`)

| Field | Default | Meaning |
|-------|---------|---------|
| `max_unknown_frames` | `32` | Max frames buffered for a flow before it is bound. |
| `max_unknown_octets` | `262144` (256 KiB) | Max total bytes buffered for unbound flows. |

#### `roqr::gateway::ReconnectPolicy`

Governs gateway auto-reconnect. Present on both `IngestOptions::reconnect` and
`EgressOptions::reconnect`.

| Field | Default | Meaning |
|-------|---------|---------|
| `initial_backoff` | `250` ms | Backoff after the first failed connect attempt; doubles each attempt. |
| `max_backoff` | `5000` ms | Cap on the exponential backoff. |
| `max_attempts` | `10` | Consecutive failed attempts before giving up (surfaces a permanent-failure state). `0` = never give up. A successful connection resets the counter. |
| `connect_timeout` | `5000` ms | Per-attempt wait for the connection to come up before counting a failure. |

A drop after a successful session triggers an immediate reconnect (no
backoff); backoff applies only between failed attempts.

#### `roqr::gateway::IngestOptions`

| Field | Default | Meaning |
|-------|---------|---------|
| `rtmp_port` | `1935` | RTMP listen port for the publisher. |
| `roqr_host` | `"127.0.0.1"` | Relay host. |
| `roqr_port` | `4443` | Relay QUIC port. |
| `insecure_skip_verify` | `true` | Skip relay certificate verification. Testing only. |
| `reconnect` | defaults above | Reconnect policy. |
| `idle_timeout` | `15000` ms | Passed to `ClientOptions::idle_timeout`. |

#### `roqr::gateway::EgressOptions`

| Field | Default | Meaning |
|-------|---------|---------|
| `rtmp_port` | `1936` | RTMP listen port for the player. |
| `roqr_host` | `"127.0.0.1"` | Relay host. |
| `roqr_port` | `4443` | Relay QUIC port. |
| `stream_name` | `"cam"` | Stream to play from the relay. |
| `insecure_skip_verify` | `true` | Skip relay certificate verification. Testing only. |
| `reconnect` | defaults above | Reconnect policy. |
| `idle_timeout` | `15000` ms | Passed to `ClientOptions::idle_timeout`. |

#### `roqr::relayd::ServerOptions`

| Field | Default | Meaning |
|-------|---------|---------|
| `port` | `0` | QUIC listen port (`0` lets the OS choose; `roqr-relayd` defaults it to `4443`). |
| `cert_file` | `""` | Server certificate (PEM), required. |
| `key_file` | `""` | Server private key (PEM), required. |
| `mode` | `Echo` | `Echo`, `Relay`, or `Media`. |
| `alpn` | `"roqr"` | QUIC ALPN identifier. |
| `close_after_ready` | `false` | Test-only: drop each connection immediately after the handshake (used to exercise client reconnect). Do not enable in production. |

### Delivery modes

`roqr::quic::DeliveryMode` selects how a frame is sent:

- `Stream` — always on a QUIC stream (reliable, ordered).
- `Datagram` — in a QUIC DATAGRAM when the extension is negotiated and the frame fits; otherwise handled per `DatagramFallback`.
- `Auto` — keeps session-correctness traffic (commands, control, metadata, shared objects) on streams and sends audio (type 8), video (type 9), and aggregate (type 22) media as datagrams when negotiated. This is the policy the gateways use for media.
