#include "roqr/roqr_rtmp.h"

#include <chrono>
#include <new>
#include <string>

#include "roqr/gateway/egress.hpp"
#include "roqr/gateway/ingest.hpp"

struct roqr_ingest {
    roqr::gateway::IngestGateway gateway;
};
struct roqr_egress {
    roqr::gateway::EgressGateway gateway;
};

extern "C" {

roqr_ingest* roqr_ingest_create(void) {
    return new (std::nothrow) roqr_ingest();
}
void roqr_ingest_destroy(roqr_ingest* in) { delete in; }

roqr_error roqr_ingest_start(roqr_ingest* in, uint16_t rtmp_port,
                              const char* roqr_host, uint16_t roqr_port,
                              int insecure_skip_verify) {
    if (in == nullptr || roqr_host == nullptr) return ROQR_ERR_INVALID_ARG;
    roqr::gateway::IngestOptions o;
    o.rtmp_port = rtmp_port;
    o.roqr_host = roqr_host;
    o.roqr_port = roqr_port;
    o.insecure_skip_verify = insecure_skip_verify != 0;
    return in->gateway.start(o) ? ROQR_OK : ROQR_ERR_GENERAL;
}

int roqr_ingest_wait_publishing(roqr_ingest* in, int timeout_ms) {
    if (in == nullptr) return 0;
    return in->gateway.wait_publishing(std::chrono::milliseconds(timeout_ms))
               ? 1
               : 0;
}

void roqr_ingest_stop(roqr_ingest* in) {
    if (in != nullptr) in->gateway.stop();
}

roqr_egress* roqr_egress_create(void) {
    return new (std::nothrow) roqr_egress();
}
void roqr_egress_destroy(roqr_egress* eg) { delete eg; }

roqr_error roqr_egress_start(roqr_egress* eg, uint16_t rtmp_port,
                              const char* roqr_host, uint16_t roqr_port,
                              const char* stream_name,
                              int insecure_skip_verify) {
    if (eg == nullptr || roqr_host == nullptr || stream_name == nullptr) {
        return ROQR_ERR_INVALID_ARG;
    }
    roqr::gateway::EgressOptions o;
    o.rtmp_port = rtmp_port;
    o.roqr_host = roqr_host;
    o.roqr_port = roqr_port;
    o.stream_name = stream_name;
    o.insecure_skip_verify = insecure_skip_verify != 0;
    return eg->gateway.start(o) ? ROQR_OK : ROQR_ERR_GENERAL;
}

int roqr_egress_wait_playing(roqr_egress* eg, int timeout_ms) {
    if (eg == nullptr) return 0;
    return eg->gateway.wait_playing(std::chrono::milliseconds(timeout_ms))
               ? 1
               : 0;
}

void roqr_egress_stop(roqr_egress* eg) {
    if (eg != nullptr) eg->gateway.stop();
}

}  // extern "C"
