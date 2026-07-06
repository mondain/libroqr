//! Example RoQR client built on the libroqr C FFI (`ffi/include/roqr/roqr.h`).
//!
//! It performs the full client lifecycle: create a client, install the receive
//! and close callbacks, connect to a relay, send a handful of RoQR frames,
//! collect the frames the relay sends back, then close and destroy the client.
//!
//! Point it at a relay running in echo mode, which reflects every frame back to
//! its sender:
//!
//! ```text
//! roqr-relayd --cert cert.pem --key key.pem --port 4443 --mode echo
//! roqr-rust-client 127.0.0.1 4443
//! ```
//!
//! Finding libroqr-ffi.so at build and run time is handled by build.rs; see
//! examples/rust/README.md.

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::sync::mpsc::{self, Sender};
use std::time::Duration;

// ---------------------------------------------------------------------------
// FFI declarations — a direct transcription of roqr.h. Keep in sync with it.
// ---------------------------------------------------------------------------

/// Mirrors `struct roqr_frame`.
#[repr(C)]
struct RoqrFrame {
    flow_id: u64,
    timestamp: u64,
    message_type: u8,
    message_stream_id: u64,
    chunk_stream_id: u64,
    payload: *const u8,
    payload_len: usize,
}

/// Opaque handle (`struct roqr_client`).
#[repr(C)]
struct RoqrClient {
    _private: [u8; 0],
}

// roqr_delivery_mode
const ROQR_DELIVERY_STREAM: c_int = 0;

// roqr_error: 0 is ROQR_OK.
type RoqrError = c_int;

type MessageCb = extern "C" fn(frame: *const RoqrFrame, user_data: *mut c_void);
type ClosedCb = extern "C" fn(app_error_code: u64, user_data: *mut c_void);

extern "C" {
    fn roqr_version() -> *const c_char;
    fn roqr_client_create() -> *mut RoqrClient;
    fn roqr_client_destroy(client: *mut RoqrClient);
    fn roqr_client_set_on_message(client: *mut RoqrClient, cb: MessageCb, user_data: *mut c_void);
    fn roqr_client_set_on_closed(client: *mut RoqrClient, cb: ClosedCb, user_data: *mut c_void);
    fn roqr_client_connect(
        client: *mut RoqrClient,
        host: *const c_char,
        port: u16,
        insecure_skip_verify: c_int,
    ) -> RoqrError;
    fn roqr_client_wait_connected(client: *mut RoqrClient, timeout_ms: c_int) -> c_int;
    fn roqr_client_bind_flow(client: *mut RoqrClient, flow_id: u64);
    fn roqr_client_send(client: *mut RoqrClient, frame: *const RoqrFrame, mode: c_int) -> RoqrError;
    fn roqr_client_close(client: *mut RoqrClient, app_error_code: u64);
    fn roqr_client_wait_closed(client: *mut RoqrClient, timeout_ms: c_int) -> c_int;
}

// ---------------------------------------------------------------------------
// Callbacks. These run on the library's QUIC network thread. Per roqr.h they
// must not block and must not call destroy/wait_*; sending on an unbounded
// channel does neither.
// ---------------------------------------------------------------------------

/// One received frame, copied out of library-owned memory.
struct Received {
    flow_id: u64,
    timestamp: u64,
    message_type: u8,
    payload: Vec<u8>,
}

extern "C" fn on_message(frame: *const RoqrFrame, user_data: *mut c_void) {
    if frame.is_null() || user_data.is_null() {
        return;
    }
    // SAFETY: the library guarantees `frame` and its payload are valid for the
    // duration of this call. `user_data` is the `Sender` pointer installed in
    // `main`; it is only ever dereferenced here on the network thread and
    // outlives every callback (the client is destroyed — joining that thread —
    // before the Sender is dropped).
    let f = unsafe { &*frame };
    let tx = unsafe { &*(user_data as *const Sender<Received>) };

    let payload = if f.payload.is_null() || f.payload_len == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(f.payload, f.payload_len) }.to_vec()
    };

    let _ = tx.send(Received {
        flow_id: f.flow_id,
        timestamp: f.timestamp,
        message_type: f.message_type,
        payload,
    });
}

extern "C" fn on_closed(app_error_code: u64, _user_data: *mut c_void) {
    eprintln!("[client] connection closed (app_error_code={app_error_code})");
}

// ---------------------------------------------------------------------------

const FRAME_COUNT: u64 = 5;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let host = args.get(1).cloned().unwrap_or_else(|| "127.0.0.1".to_string());
    let port: u16 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(4443);

    // SAFETY: roqr_version returns a static NUL-terminated string.
    let version = unsafe { CStr::from_ptr(roqr_version()) }
        .to_string_lossy()
        .into_owned();
    println!("libroqr {version}: echo demo against {host}:{port}");

    // Channel carrying received frames from the network thread to main. The
    // Sender is boxed so its address stays stable for the client's lifetime.
    let (tx, rx) = mpsc::channel::<Received>();
    let tx = Box::new(tx);
    let tx_ptr = &*tx as *const Sender<Received> as *mut c_void;

    // SAFETY: roqr_client_create returns a valid handle or null.
    let client = unsafe { roqr_client_create() };
    assert!(!client.is_null(), "roqr_client_create returned null");

    // Handlers must be installed before connect.
    unsafe {
        roqr_client_set_on_message(client, on_message, tx_ptr);
        roqr_client_set_on_closed(client, on_closed, std::ptr::null_mut());
    }

    let c_host = CString::new(host.as_str()).expect("host contains a NUL byte");
    // SAFETY: valid client and NUL-terminated host string.
    let rc = unsafe { roqr_client_connect(client, c_host.as_ptr(), port, 1 /* insecure */) };
    if rc != 0 {
        eprintln!("connect failed (roqr_error={rc})");
        return cleanup(client, tx, 1);
    }
    if unsafe { roqr_client_wait_connected(client, 5000) } != 1 {
        eprintln!("timed out connecting; is a relay running on {host}:{port}?");
        return cleanup(client, tx, 1);
    }
    println!("[client] connected");

    // RoQR gates received frames by flow (draft s5): frames arriving on a flow
    // that has not been bound are buffered, not delivered to on_message. Bind
    // the flow we will use so the echoed frames come through. (Flow 0 is the
    // exception — it is always delivered.)
    const FLOW_ID: u64 = 1;
    unsafe { roqr_client_bind_flow(client, FLOW_ID) };

    // Send frames. Echo mode reflects each one back to us.
    for i in 1..=FRAME_COUNT {
        let payload = format!("hello-roqr-{i}").into_bytes();
        let frame = RoqrFrame {
            flow_id: FLOW_ID,
            timestamp: i * 40,
            message_type: 9, // video message type, arbitrary for this demo
            message_stream_id: 1,
            chunk_stream_id: 6,
            payload: payload.as_ptr(),
            payload_len: payload.len(),
        };
        // SAFETY: valid client and frame; the library copies the payload before
        // returning, so `payload` may drop at the end of this iteration.
        let rc = unsafe { roqr_client_send(client, &frame, ROQR_DELIVERY_STREAM) };
        if rc == 0 {
            println!("[client] sent #{i}: {} bytes, ts={}", payload.len(), i * 40);
        } else {
            eprintln!("[client] send #{i} failed (roqr_error={rc})");
        }
    }

    // Collect the echoes.
    let mut received = 0u64;
    while received < FRAME_COUNT {
        match rx.recv_timeout(Duration::from_secs(5)) {
            Ok(r) => {
                received += 1;
                println!(
                    "[client] echo #{received}: type={} ts={} flow={} \"{}\"",
                    r.message_type,
                    r.timestamp,
                    r.flow_id,
                    String::from_utf8_lossy(&r.payload)
                );
            }
            Err(_) => {
                eprintln!("timed out; {received}/{FRAME_COUNT} frames echoed back");
                break;
            }
        }
    }

    // SAFETY: valid client.
    unsafe {
        roqr_client_close(client, 0);
        roqr_client_wait_closed(client, 5000);
    }
    println!("[client] done: {received}/{FRAME_COUNT} frames echoed back");

    let ok = received == FRAME_COUNT;
    cleanup(client, tx, if ok { 0 } else { 1 });
}

/// Destroy the client (joins the network thread, so no further callbacks fire),
/// then drop the Sender the callbacks referenced, and exit.
fn cleanup(client: *mut RoqrClient, tx: Box<Sender<Received>>, code: i32) {
    // SAFETY: `client` came from roqr_client_create and has not been destroyed.
    unsafe { roqr_client_destroy(client) };
    drop(tx);
    std::process::exit(code);
}
