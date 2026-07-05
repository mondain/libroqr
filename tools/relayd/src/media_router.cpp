#include "roqr/relayd/media_router.hpp"

#include <algorithm>

namespace roqr::relayd {

void MediaRouter::register_publisher(void* conn,
                                     const std::string& stream_name) {
    streams_[stream_name].publisher = conn;
    conn_stream_[conn] = stream_name;
}

void MediaRouter::register_subscriber(void* conn,
                                      const std::string& stream_name) {
    auto& s = streams_[stream_name];
    if (std::find(s.subscribers.begin(), s.subscribers.end(), conn) ==
        s.subscribers.end()) {
        s.subscribers.push_back(conn);
    }
    conn_stream_[conn] = stream_name;
}

std::vector<void*> MediaRouter::subscribers_for_publisher(void* conn) const {
    auto it = conn_stream_.find(conn);
    if (it == conn_stream_.end()) return {};
    auto sit = streams_.find(it->second);
    if (sit == streams_.end() || sit->second.publisher != conn) return {};
    return sit->second.subscribers;
}

bool MediaRouter::is_publisher(void* conn) const {
    auto it = conn_stream_.find(conn);
    if (it == conn_stream_.end()) return false;
    auto sit = streams_.find(it->second);
    if (sit == streams_.end()) return false;
    return sit->second.publisher == conn;
}

void MediaRouter::cache_init(const std::string& stream_name,
                             uint8_t message_type,
                             std::vector<uint8_t> frame_bytes) {
    auto& s = streams_[stream_name];
    if (s.init.find(message_type) == s.init.end()) {
        s.init_types.push_back(message_type);
    }
    s.init[message_type] = std::move(frame_bytes);
}

std::vector<std::vector<uint8_t>> MediaRouter::init_frames(
    const std::string& stream_name) const {
    auto it = streams_.find(stream_name);
    if (it == streams_.end()) return {};
    std::vector<std::vector<uint8_t>> out;
    for (uint8_t type : it->second.init_types) {
        out.push_back(it->second.init.at(type));
    }
    return out;
}

void MediaRouter::remove(void* conn) {
    auto it = conn_stream_.find(conn);
    if (it == conn_stream_.end()) return;
    auto sit = streams_.find(it->second);
    if (sit != streams_.end()) {
        Stream& s = sit->second;
        if (s.publisher == conn) s.publisher = nullptr;
        s.subscribers.erase(
            std::remove(s.subscribers.begin(), s.subscribers.end(), conn),
            s.subscribers.end());
    }
    conn_stream_.erase(it);
}

std::string MediaRouter::stream_of(void* conn) const {
    auto it = conn_stream_.find(conn);
    return it == conn_stream_.end() ? std::string() : it->second;
}

}  // namespace roqr::relayd
