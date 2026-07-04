#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace roqr::relayd {

// Echo: reflect every RoQR frame back to its sender on the same carriage
// (stream frames on the same stream, datagram frames as datagrams).
// Relay: forward frames to all other connections (Task 8; until then Relay
// behaves as Echo).
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
