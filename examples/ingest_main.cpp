#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "roqr/gateway/ingest.hpp"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }
}  // namespace

int main(int argc, char** argv) {
    roqr::gateway::IngestOptions o;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--rtmp-port") && i + 1 < argc) {
            o.rtmp_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--roqr-host") && i + 1 < argc) {
            o.roqr_host = argv[++i];
        } else if (!std::strcmp(argv[i], "--roqr-port") && i + 1 < argc) {
            o.roqr_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else {
            std::fprintf(stderr,
                         "usage: roqr-ingest [--rtmp-port P] "
                         "[--roqr-host H] [--roqr-port P]\n");
            return 2;
        }
    }
    roqr::gateway::IngestGateway g;
    if (!g.start(o)) {
        std::fprintf(stderr, "ingest: failed to start\n");
        return 1;
    }
    std::printf("roqr-ingest: RTMP :%u -> RoQR %s:%u\n", o.rtmp_port,
                o.roqr_host.c_str(), o.roqr_port);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (!g_stop) {
        struct timespec ts {0, 200'000'000};
        nanosleep(&ts, nullptr);
    }
    g.stop();
    return 0;
}
