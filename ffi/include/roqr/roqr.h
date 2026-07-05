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

#ifdef __cplusplus
}
#endif

#endif /* ROQR_H */
