# libroqr Design

Date: 2026-07-04
Status: Approved
Draft implemented: draft-gregoire-rtmp-over-quic (RoQR), text source at
`../roqr/draft-gregoire-rtmp-over-quic.txt`

## Goal

A C++20 client library implementing the RoQR mapping (RTMP over QUIC) with a C
FFI, JNI bindings, and native example applications that connect to a
RoQR-implementing server as bidirectional full-duplex clients. The reference
media path is:

```
ffmpeg (RTMP publish) -> roqr-ingest -> RoQR server -> roqr-egress -> ffmpeg/ffplay (RTMP subscribe)
```

RoQR is the intermediary that lets RTMP flow over QUIC. The primary external
target is a RoQR server such as Red5 Pro; the repo also ships a minimal test
relay so everything runs standalone.

## Decisions

- QUIC stack: picoquic (+picotls, OpenSSL backend). Not abstracted behind an
  adapter interface; the sans-I/O core keeps the wire format testable without
  it.
- Architecture: sans-I/O protocol core plus an integrated picoquic transport
  layer (Approach A). A full adapter architecture (moq5-style) was considered
  and rejected as premature for a mapping this thin; direct-only integration
  was rejected because codec bugs must be falsifiable in unit tests.
- RTMP/AMF lives in an optional library module (`roqr-rtmp`), not just in
  examples, so FFI/JNI consumers get gateway capability. librtmp is not
  linked; ffmpeg and librtmp source trees serve as correctness references.
- JNI targets desktop JVM first with Android NDK build support included.
- Validation: real ffmpeg processes drive end-to-end tests; unit tests cover
  the codecs.
- Test server included: `tools/roqr-relayd`, a minimal RoQR relay.
- E-RTMP (Veovera Enhanced RTMP v1/v2) is supported at the gateway
  classification level. Reference copies live in `docs/reference/`
  (`enhanced-rtmp-v1.md`, `enhanced-rtmp-v2.md`).

## Repository Layout

```
libroqr/
  CMakeLists.txt        # options: ROQR_BUILD_RTMP/FFI/JNI/EXAMPLES/TOOLS/TESTS
  CMakePresets.json
  cmake/                # FindPicoquic.cmake (installed pkg, source dir, FetchContent)
  core/                 # roqr-core: sans-I/O protocol (static lib)
  quic/                 # roqr-quic: picoquic client transport
  rtmp/                 # roqr-rtmp: handshake, chunking, AMF0
  ffi/                  # roqr shared lib: C API roqr.h, roqr_rtmp.h
  jni/                  # roqr-jni glue + Java sources (org.red5.roqr), jar via UseJava
  examples/             # roqr-ingest, roqr-egress, roqr-duplex, C and Java samples
  tools/                # roqr-relayd test server
  tests/                # Catch2 unit tests, loopback integration, ffmpeg e2e script
  docs/
```

C++ namespace `roqr::`, C symbol prefix `roqr_`, Java package `org.red5.roqr`.
License: Apache-2.0.
Toolchain: C++20, CMake >= 3.24 with presets, mirroring moq5 conventions.
picoquic resolution order: installed config package, `-DROQR_PICOQUIC_SOURCE_DIR`,
FetchContent. Android TLS uses the existing openssl-android builds.

## Components

### roqr-core (sans-I/O)

- `Varint`: QUIC variable-length integer encode/decode (RFC 9000 s16).
- `Frame`: RoQR frame per draft s7.2 (Flow ID, Timestamp, Message Type,
  Message Stream ID, Chunk Stream ID, Payload Length, Payload). Encoder plus
  an incremental decoder that tolerates partial frames across stream reads
  (draft s7.4). Datagram decode requires exactly one complete frame, no
  trailing bytes (s7.5).
- Validation: payload length must be > 0, one complete RTMP message per
  frame, no split or concatenated payloads (s7.2). Violations map to draft
  Table 2 error codes.
- `FlowTable`: flow states (active, retired), monotonic Flow ID reuse rule
  (s5), bounded unknown-flow buffering capped by both frame count and octet
  count (s5); overflow signals stream-stop with UNKNOWN_FLOW_ID or datagram
  drop.
- Unknown RTMP message types pass through to the application untouched (s9).

### roqr-quic (picoquic transport)

- `Client`: `connect(host, port, options)` with ALPN `roqr`; negotiates the
  QUIC DATAGRAM extension; 0-RTT off by default (s13).
- One I/O thread per client running the picoquic packet loop. Callbacks
  (`on_connected`, `on_message`, `on_datagram_gap_hint`, `on_closed`) fire on
  that thread. `send(flow_id, msg, mode)` is thread-safe via queue plus loop
  wakeup.
- Delivery modes: `Stream`, `Datagram`, `Auto`. Auto sends command, control,
  metadata, and decoder-config messages on streams; audio/video go in
  datagrams only when the extension is negotiated and the encoded frame fits
  the current max datagram size, else on the stream (s7.5, s10).
- Default wiring: Flow 0 on one bidirectional stream carries the RTMP
  session (s5). API supports binding additional Flow IDs to their own
  streams; examples ship on Flow 0 for simplest interop.
- `close(error_code)` and `reset_flow_stream()` (FRAME_CANCELLED, s8) exposed.

### roqr-rtmp (gateway support)

- Simple RTMP handshake, responder and initiator roles.
- `ChunkReader`/`ChunkWriter`: fmt 0-3 headers, Set Chunk Size, Abort,
  extended timestamps. Extended-timestamp sentinel resolved to the full
  message timestamp before RoQR encoding and regenerated when re-chunking
  (s7.3).
- AMF0 codec: number, boolean, string, object, null, undefined, ECMA array,
  strict array, date - the types RTMP command traffic uses.
- `ServerSession`: TCP accept, handshake, connect/createStream/publish/play
  command handling, yields complete de-chunked RTMP messages. POSIX TCP,
  thread per connection; gateway-grade, not a general server framework.
- Media classifier (E-RTMP aware): inspects audio/video message headers to
  classify each message for delivery-mode and gap-recovery decisions.
  Handles legacy FLV tag headers (codec id nibble, AVC/AAC sequence
  headers, keyframe flag) and Enhanced RTMP v1/v2 ex-headers: the IsExHeader
  bit, VideoPacketType (SequenceStart, CodedFrames, SequenceEnd,
  CodedFramesX, Metadata, MPEG2TSSequenceStart, Multitrack, ModEx),
  AudioPacketType (SequenceStart, CodedFrames, SequenceEnd,
  MultichannelConfig, Multitrack, ModEx), and video/audio FourCC. ModEx
  blocks are skipped to reach the effective packet type; payload bytes are
  never modified. Classification output: {sequence-header | metadata |
  keyframe | coded | control} plus fourcc/codec id. E-RTMP `connect`
  capability objects (capsEx, fourCcList, videoFourCcInfoMap) pass through
  the gateway untouched.

### FFI (`roqr` shared library)

Opaque handles, message struct with pointer+length payload, function-pointer
callbacks with `user_data`, error enum mirroring draft Table 2, version query.
`roqr.h` covers the RoQR client; `roqr_rtmp.h` exposes the RTMP listener and
session when ROQR_BUILD_RTMP is on. No C++ types cross the ABI.

### JNI (`roqr-jni`)

`RoqrClient`, `RtmpMessage`, `DeliveryMode`, listener interface; direct
ByteBuffers for payloads. Desktop JVM build via CMake FindJNI + UseJava jar;
Android via NDK toolchain file.

### Examples

- `roqr-ingest`: RTMP listener on :1935; ffmpeg publishes to it; forwards the
  session over RoQR to the server.
- `roqr-egress`: RoQR client that plays a stream and serves it as an RTMP
  listener on :1936 for ffplay/ffmpeg.
- `roqr-duplex`: publish and subscribe over one QUIC connection,
  demonstrating bidirectional full-duplex operation.
- Minimal C (FFI) and Java (JNI) client samples.

### tools/roqr-relayd

picoquic server with ALPN `roqr`. Speaks just enough AMF0 to answer
`connect`/`createStream` with `_result` and `publish`/`play` with `onStatus`,
then relays media frames from publisher to subscribers keyed by stream name,
preserving each frame's delivery mode. Exists so examples, integration tests,
and CI run without an external server.

## Data Flow

Publish: ffmpeg -> `rtmp://ingest:1935/live/<name>`. ServerSession handshakes
and dechunks. Commands travel as RoQR frames on Flow 0's bidirectional
stream; payload bytes are untouched, metadata is lifted into the frame
header. Responses from the server are re-chunked back to ffmpeg. After
publish is accepted: sequence headers (legacy AVC/AAC config or E-RTMP
SequenceStart/MultichannelConfig), onMetaData, and E-RTMP
VideoPacketType.Metadata always go on stream; SequenceEnd on stream;
CodedFrames/CodedFramesX and legacy coded frames follow the configured mode
(Auto default). Multitrack messages are classified conservatively: if any
track in the message is a sequence header or metadata, the whole message
goes on stream. Gap-recovery keyframe detection understands both legacy
frame-type nibbles and E-RTMP ex-headers.

Subscribe: roqr-egress connects, issues connect/createStream/play on Flow 0,
receives media frames, re-chunks, serves ffplay. RoQR frames carry no
sequence numbers, so datagram gaps are detected heuristically from RTMP
timestamp discontinuities per flow and message type; on a suspected gap the
timeline is discontinuous until the next random access point plus decoder
config (s8), and output resumes from there.

## Error Handling

- Malformed stream-carried frame: close connection FRAME_ENCODING_ERROR.
  Malformed datagram: drop, count, continue (s12).
- Unknown Flow ID: bounded buffering (configurable caps); overflow ->
  STOP_SENDING with UNKNOWN_FLOW_ID for streams, drop for datagrams (s5).
- Stale stream media: optional reset with FRAME_CANCELLED (s8, s11).
- Oversized datagram in Datagram mode: policy fallback-to-stream (default)
  or drop (s7.5).
- All eight Table 2 error codes exposed in C++, FFI, and Java enums.
- RTMP-side protocol violations close only the TCP connection, never the
  QUIC connection (trust boundary, s14).
- FFI: status returns plus on_error callback. JNI: exceptions for API
  misuse, listener callbacks for async errors.

## Testing

Unit (Catch2 v3 via CTest):
- varint edge vectors; frame codec round-trip; truncation and malformed
  cases including zero payload length; incremental decode across arbitrary
  split points.
- chunk reader/writer round-trips: fmt 0-3, mid-stream Set Chunk Size,
  extended timestamps (>= 0xFFFFFF).
- AMF0 round-trip against byte vectors captured from ffmpeg output.
- flow-table limit enforcement.
- media classifier vectors: legacy AVC/AAC tags and E-RTMP v1/v2 ex-headers
  (hvc1/av01/vp09 video, ac-3/opus/flac audio, ModEx-prefixed packets,
  multitrack messages), asserting classification and fourcc extraction.

Integration:
- In-process loopback client <-> relayd: publish/play, both delivery modes,
  simulated datagram loss, error-code paths.
- End-to-end script: ffmpeg lavfi testsrc+sine -> ingest -> relayd ->
  egress -> ffmpeg remux to file; assert stream-copy integrity (packet
  counts, codec parameters, H.264 bitstream hash). A second, E-RTMP case
  publishes HEVC over enhanced RTMP (skipped automatically when the
  installed ffmpeg lacks enhanced-FLV support).

CI (GitHub Actions): gcc/clang build matrix; unit plus loopback always;
ffmpeg e2e on ubuntu runner. Manual interop target: point examples at a
Red5 Pro RoQR server.

## Out of Scope (initial release)

- RoQR server beyond the minimal test relay.
- RTMP client-initiator gateway push to legacy RTMP servers (initiator
  handshake ships, a push example does not).
- AMF3, shared object semantics, aggregate message unpacking (payloads pass
  through opaquely).
- E-RTMP multitrack fan-out to separate RoQR Flow IDs (multitrack messages
  are classified and relayed whole); interpretation of ModEx
  TimestampOffsetNano beyond skipping it during classification; the v2
  reconnect-request flow.
- 0-RTT support, connection migration handling beyond picoquic defaults.
- Encoder rate adaptation; the library exposes transport signals only.
