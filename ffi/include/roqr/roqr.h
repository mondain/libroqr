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
