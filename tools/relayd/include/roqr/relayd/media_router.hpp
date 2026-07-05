#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace roqr::relayd {

class MediaRouter {
public:
    void register_publisher(void* conn, const std::string& stream_name);
    void register_subscriber(void* conn, const std::string& stream_name);
    std::vector<void*> subscribers_for_publisher(void* conn) const;
    void cache_init(const std::string& stream_name, uint8_t message_type,
                    std::vector<uint8_t> frame_bytes);
    std::vector<std::vector<uint8_t>> init_frames(
        const std::string& stream_name) const;
    void remove(void* conn);
    std::string stream_of(void* conn) const;

private:
    struct Stream {
        void* publisher = nullptr;
        std::vector<void*> subscribers;
        std::vector<uint8_t> init_types;              // insertion order
        std::map<uint8_t, std::vector<uint8_t>> init;  // by message type
    };
    std::map<std::string, Stream> streams_;
    std::map<void*, std::string> conn_stream_;  // conn -> stream name
};

}  // namespace roqr::relayd
