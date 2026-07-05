#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "roqr/gateway/egress.hpp"
#include "roqr/gateway/ingest.hpp"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }
}  // namespace

int main(int argc, char** argv) {
    roqr::gateway::IngestOptions in;
    roqr::gateway::EgressOptions eg;
    in.rtmp_port = 1935;
    eg.rtmp_port = 1936;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--roqr-host") && i + 1 < argc) {
            in.roqr_host = eg.roqr_host = argv[++i];
        } else if (!std::strcmp(argv[i], "--roqr-port") && i + 1 < argc) {
            in.roqr_port = eg.roqr_port =
                static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--stream") && i + 1 < argc) {
            eg.stream_name = argv[++i];
        } else {
            std::fprintf(stderr,
                         "usage: roqr-duplex [--roqr-host H] "
                         "[--roqr-port P] [--stream NAME]\n");
            return 2;
        }
    }
    roqr::gateway::IngestGateway ingest;
    roqr::gateway::EgressGateway egress;
    if (!ingest.start(in) || !egress.start(eg)) {
        std::fprintf(stderr, "duplex: failed to start\n");
        return 1;
    }
    std::printf("roqr-duplex: ingest RTMP :%u, egress RTMP :%u, RoQR %s:%u\n",
                in.rtmp_port, eg.rtmp_port, in.roqr_host.c_str(),
                in.roqr_port);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (!g_stop) {
        struct timespec ts {0, 200'000'000};
        nanosleep(&ts, nullptr);
    }
    ingest.stop();
    egress.stop();
    return 0;
}
