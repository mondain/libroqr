#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "roqr/gateway/egress.hpp"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }
}  // namespace

int main(int argc, char** argv) {
    roqr::gateway::EgressOptions o;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--rtmp-port") && i + 1 < argc) {
            o.rtmp_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--roqr-host") && i + 1 < argc) {
            o.roqr_host = argv[++i];
        } else if (!std::strcmp(argv[i], "--roqr-port") && i + 1 < argc) {
            o.roqr_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--stream") && i + 1 < argc) {
            o.stream_name = argv[++i];
        } else {
            std::fprintf(stderr,
                         "usage: roqr-egress [--rtmp-port P] "
                         "[--roqr-host H] [--roqr-port P] [--stream NAME]\n");
            return 2;
        }
    }
    roqr::gateway::EgressGateway g;
    if (!g.start(o)) {
        std::fprintf(stderr, "egress: failed to start\n");
        return 1;
    }
    std::printf("roqr-egress: RTMP :%u <- RoQR %s:%u (stream: %s)\n", o.rtmp_port,
                o.roqr_host.c_str(), o.roqr_port, o.stream_name.c_str());
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (!g_stop) {
        struct timespec ts {0, 200'000'000};
        nanosleep(&ts, nullptr);
    }
    g.stop();
    return 0;
}
