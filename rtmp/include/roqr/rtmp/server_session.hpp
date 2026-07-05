#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "roqr/rtmp/message.hpp"

namespace roqr::rtmp {

class ServerSession;

// All callbacks fire on the session's own thread and must not block.
struct SessionEvents {
    std::function<void(ServerSession&, const RtmpMessage&)> on_message;
    std::function<void(ServerSession&, const std::string& app)> on_connect;
    std::function<void(ServerSession&, const std::string& stream_name,
                       bool publishing)> on_stream;
    std::function<void(ServerSession&)> on_close;
};

class ServerSession {
public:
    ServerSession(int fd, SessionEvents events);
    ~ServerSession();  // closes the socket
    void run();        // blocking: handshake, dechunk, dispatch until close
    bool send(const RtmpMessage& msg);  // thread-safe (chunks + writes)
    void close();      // shutdown(fd); run() returns soon after
    // Thread-safe; values reflect the session thread's latest state.
    std::string app() const;
    std::string stream_name() const;
    bool publishing() const;

    // Must be called before run(); Listener uses this so the events
    // factory can capture a reference to the constructed session.
    void set_events(SessionEvents events);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Listener {
public:
    using EventsFactory = std::function<SessionEvents(ServerSession&)>;
    Listener();
    ~Listener();  // calls stop()
    bool start(uint16_t port, EventsFactory factory);
    void stop();  // stop accepting, close sessions, join threads
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace roqr::rtmp
