#pragma once

#include <cstdint>

#include "roqr/frame.hpp"
#include "roqr/rtmp/message.hpp"

namespace roqr::gateway {

// Widen an RTMP message into a RoQR frame on the given flow. All RTMP
// metadata fits in the wider RoQR fields, so this never fails. The RoQR
// timestamp carries the fully resolved RTMP message timestamp (draft
// s7.3); the caller must not send a frame with an empty payload (RoQR
// requires Payload Length > 0).
roqr::Frame to_frame(const roqr::rtmp::RtmpMessage& msg, uint64_t flow_id);

// Narrow a RoQR frame back into an RTMP message. Returns false and leaves
// out untouched if timestamp, message_stream_id, or chunk_stream_id exceeds
// 0xFFFFFFFF (the Width bridge rule).
bool to_rtmp(const roqr::Frame& frame, roqr::rtmp::RtmpMessage& out);

}  // namespace roqr::gateway
