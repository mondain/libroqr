#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace roqr::relayd {

// Echo: reflect every RoQR frame back to its sender on the same carriage.
// Relay: forward every RoQR frame to all other live connections, preserving
// Flow ID, stream id, and carriage; the sender does not get it back.
enum class Mode { Echo, Relay };

struct ServerOptions {
    uint16_t port = 0;
    std::string cert_file;
    std::string key_file;
    Mode mode = Mode::Echo;
    std::string alpn = "roqr";
};

class Server {
public:
    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(const ServerOptions& options);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace roqr::relayd
