#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace roqr::relayd {

// Echo: reflect every RoQR frame back to its sender on the same carriage.
// Relay: forward every RoQR frame to all other live connections, preserving
// Flow ID and carriage; the sender does not get it back. Stream ids are NOT
// preserved: each (source connection, source stream) pair gets its own
// server-initiated stream toward each destination.
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
