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
`cmake -S . -B build -DROQR_BUILD_QUIC=OFF -DROQR_BUILD_EXAMPLES=OFF && cmake --build build`.

## Layout

- `core/` sans-I/O RoQR frame codec, flow table (no dependencies)
- `quic/` picoquic client transport
- `rtmp/` RTMP handshake, chunking, AMF0, E-RTMP media classifier
- `gateway/` RTMP<->RoQR bridge, ingest/egress gateways
- `tools/relayd/` the RoQR test relay
- `examples/` roqr-ingest, roqr-egress, roqr-duplex

License: Apache-2.0.
