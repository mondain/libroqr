#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "roqr/relayd/server.hpp"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void handle_signal(int) { g_stop = 1; }
}  // namespace

int main(int argc, char** argv) {
    roqr::relayd::ServerOptions opts;
    opts.port = 4443;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            opts.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--cert") == 0 && i + 1 < argc) {
            opts.cert_file = argv[++i];
        } else if (std::strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            opts.key_file = argv[++i];
        } else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            ++i;
            if (std::strcmp(argv[i], "relay") == 0) {
                opts.mode = roqr::relayd::Mode::Relay;
            } else if (std::strcmp(argv[i], "media") == 0) {
                opts.mode = roqr::relayd::Mode::Media;
            } else {
                opts.mode = roqr::relayd::Mode::Echo;
            }
        } else {
            std::fprintf(stderr,
                         "usage: roqr-relayd --cert C --key K [--port P] "
                         "[--mode echo|relay|media]\n");
            return 2;
        }
    }
    if (opts.cert_file.empty() || opts.key_file.empty()) {
        std::fprintf(stderr, "error: --cert and --key are required\n");
        return 2;
    }

    roqr::relayd::Server server;
    if (!server.start(opts)) {
        std::fprintf(stderr, "error: failed to start server\n");
        return 1;
    }
    std::printf("roqr-relayd listening on port %u\n", opts.port);
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    while (g_stop == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    server.stop();
    return 0;
}
