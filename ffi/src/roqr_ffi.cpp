#include "roqr/roqr.h"

#include <memory>
#include <new>

#include "roqr/quic/client.hpp"

// The opaque roqr_client owns a roqr::quic::Client plus the C callback
// pointers registered against it (added in Task 2).
struct roqr_client {
    roqr::quic::Client client;
};

extern "C" {

const char* roqr_version(void) { return "0.1.0"; }

roqr_client* roqr_client_create(void) {
    return new (std::nothrow) roqr_client();
}

void roqr_client_destroy(roqr_client* client) { delete client; }

}  // extern "C"
