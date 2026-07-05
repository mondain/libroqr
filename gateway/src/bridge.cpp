#include "roqr/gateway/bridge.hpp"

namespace roqr::gateway {

roqr::Frame to_frame(const roqr::rtmp::RtmpMessage& msg, uint64_t flow_id) {
    roqr::Frame f;
    f.flow_id = flow_id;
    f.timestamp = msg.timestamp;
    f.message_type = msg.type;
    f.message_stream_id = msg.message_stream_id;
    f.chunk_stream_id = msg.chunk_stream_id;
    f.payload = msg.payload;
    return f;
}

bool to_rtmp(const roqr::Frame& frame, roqr::rtmp::RtmpMessage& out) {
    constexpr uint64_t kU32Max = 0xFFFFFFFF;
    if (frame.timestamp > kU32Max || frame.message_stream_id > kU32Max ||
        frame.chunk_stream_id > kU32Max) {
        return false;
    }
    out.timestamp = static_cast<uint32_t>(frame.timestamp);
    out.type = frame.message_type;
    out.message_stream_id = static_cast<uint32_t>(frame.message_stream_id);
    out.chunk_stream_id = static_cast<uint32_t>(frame.chunk_stream_id);
    out.payload = frame.payload;
    return true;
}

}  // namespace roqr::gateway
