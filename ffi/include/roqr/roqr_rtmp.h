#ifndef ROQR_RTMP_H
#define ROQR_RTMP_H

#include <stdint.h>

#include "roqr/roqr.h" /* roqr_error */

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
