# RoQR Rust client example

A small Rust program that drives a RoQR client through the libroqr C FFI
(`ffi/include/roqr/roqr.h`). It performs the full client lifecycle and prints
what it sends and receives:

1. `roqr_client_create`
2. `roqr_client_set_on_message` / `roqr_client_set_on_closed`
3. `roqr_client_connect` + `roqr_client_wait_connected`
4. `roqr_client_bind_flow` (so echoed frames on the flow are delivered)
5. `roqr_client_send` (a few frames)
6. receives the echoes in the on-message callback
7. `roqr_client_close` + `roqr_client_wait_closed`
8. `roqr_client_destroy`

It has no third-party crates — only the Rust standard library and the C ABI.
The FFI declarations in `src/main.rs` are a direct transcription of `roqr.h`.

## Prerequisites

- A Rust toolchain (`cargo`, `rustc`).
- The libroqr FFI shared library built. From the repository root:

  ```
  eval "$(scripts/setup_picoquic_deps.sh)"
  cmake --preset dev
  cmake --build --preset dev
  ```

  This produces `build/dev/ffi/libroqr-ffi.so`, which is where `build.rs`
  looks by default. To use a different location (for example an installed
  `lib/` directory), set `ROQR_FFI_LIB_DIR` to the directory containing
  `libroqr-ffi.so`:

  ```
  ROQR_FFI_LIB_DIR=/usr/local/lib cargo build
  ```

`build.rs` bakes an rpath to that directory, so the built binary runs without
setting `LD_LIBRARY_PATH`.

## Build

```
cargo build
```

## Run

The example connects to a RoQR relay in **echo** mode, which reflects every
frame back to its sender. Start one (generate a self-signed cert first if you
do not have one):

```
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
  -keyout key.pem -out cert.pem -days 365 -nodes -subj "/CN=localhost"

roqr-relayd --cert cert.pem --key key.pem --port 4443 --mode echo
```

Then run the client (defaults to `127.0.0.1:4443`):

```
cargo run                       # or: ./target/debug/roqr-rust-client
cargo run -- 127.0.0.1 4443     # explicit host and port
```

Expected output:

```
libroqr 0.1.0: echo demo against 127.0.0.1:4443
[client] connected
[client] sent #1: 12 bytes, ts=40
...
[client] echo #1: type=9 ts=40 flow=1 "hello-roqr-1"
...
[client] done: 5/5 frames echoed back
```

The process exits 0 when all frames are echoed back, non-zero otherwise.

## Notes

- **Callback threading.** `on_message` and `on_closed` run on the library's
  QUIC network thread. Per `roqr.h` they must not block and must not call
  `destroy` or the `wait_*` functions. This example forwards each received
  frame over a `std::sync::mpsc` channel (a non-blocking send) and does the
  printing on the main thread.
- **Frame payload lifetime.** In the receive callback, `frame->payload` points
  into library-owned memory valid only for that call, so the example copies it.
  On send, the library copies the payload before `roqr_client_send` returns, so
  the Rust-side buffer can be freed immediately after.
- **Flow gating.** RoQR buffers received frames on an unbound flow until
  `roqr_client_bind_flow` (draft s5). The example binds flow 1 before sending;
  flow 0 is always delivered without binding.
- **TLS.** The example passes `insecure_skip_verify = 1`, which skips server
  certificate verification — fine for this local demo, but do not use it
  against an untrusted network.
