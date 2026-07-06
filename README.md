# libroqr

A C++20 implementation of RoQR (RTMP over QUIC,
draft-gregoire-rtmp-over-quic) with a sans-I/O protocol core, a picoquic
transport, an RTMP/AMF gateway module, a test relay, and example gateways.

## Reference media path

```
ffmpeg (RTMP publish) -> roqr-ingest -> roqr-relayd -> roqr-egress -> ffmpeg (RTMP play)
```

RoQR carries RTMP message metadata and payloads over QUIC streams and
DATAGRAM frames. The gateways auto-reconnect to the relay if the QUIC
connection drops.

## Documentation

- [docs/guide.md](docs/guide.md) — development, deployment, and the full
  configuration reference (build options, CLI flags, and library options).
- `cmake/android-jni.md` — Android NDK cross-compile for the JNI bindings.
- `docs/reference/` — vendored Enhanced RTMP (E-RTMP) specifications.

## Build

```
eval "$(scripts/setup_picoquic_deps.sh)"   # clone + build pinned picoquic/picotls
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The core protocol library builds without picoquic:
`cmake -S . -B build -DROQR_BUILD_QUIC=OFF -DROQR_BUILD_EXAMPLES=OFF && cmake --build build`.

## Layout

- `core/` sans-I/O RoQR frame codec, flow table (no dependencies)
- `quic/` picoquic client transport
- `rtmp/` RTMP handshake, chunking, AMF0, E-RTMP media classifier
- `gateway/` RTMP<->RoQR bridge, ingest/egress gateways
- `tools/relayd/` the RoQR test relay
- `examples/` roqr-ingest, roqr-egress, roqr-duplex

## Language bindings

- `ffi/` a C ABI (`roqr.h` for the client, `roqr_rtmp.h` for the ingest/egress
  gateways) in `libroqr-ffi.so`.
- `jni/` JNI bindings (`org.red5.roqr`) in `libroqr-jni.so` + `roqr.jar`,
  built when `-DROQR_BUILD_JNI=ON` and a JDK is present.
- `examples/java/` Java publish/play samples (see `examples/java/README.md`).
- Android: see `cmake/android-jni.md` for the NDK cross-compile.

Native callbacks (`MessageListener`) fire on a native thread the binding
attaches to the JVM for the call; do not block in them.

License: Apache-2.0.
