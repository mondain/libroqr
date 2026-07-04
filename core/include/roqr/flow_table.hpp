#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "roqr/frame.hpp"

namespace roqr {

enum class FlowState { Unknown, Active, Retired };

struct FlowTableLimits {
    size_t max_unknown_frames = 32;
    size_t max_unknown_octets = 256 * 1024;
};

// Tracks Flow ID lifecycle (draft s5). Flow 0, the default RTMP session
// flow, starts Active. Frames for flows that are not yet Active can be
// buffered within bounded limits; the transport decides whether overflow
// stops a stream (UNKNOWN_FLOW_ID) or drops datagrams.
class FlowTable {
public:
    explicit FlowTable(FlowTableLimits limits = {});

    // Binds a flow to application state. Returns false if the ID was
    // retired earlier: Flow IDs MUST NOT be reused for unrelated media
    // (draft s5), enforced here as never-reuse-within-a-connection.
    bool activate(uint64_t flow_id);
    void retire(uint64_t flow_id);
    FlowState state(uint64_t flow_id) const;

    enum class BufferResult { Buffered, LimitExceeded };

    // Buffers a frame for a not-yet-Active flow. Enforces both the frame
    // count and octet limits (draft s5); on LimitExceeded the frame is not
    // buffered.
    BufferResult buffer_unknown(Frame frame);

    // Drains and returns frames buffered for flow_id, preserving arrival
    // order. Called on activation.
    std::vector<Frame> take_buffered(uint64_t flow_id);

private:
    FlowTableLimits limits_;
    std::unordered_map<uint64_t, FlowState> states_;
    std::deque<Frame> unknown_;
    size_t unknown_octets_ = 0;
};

}  // namespace roqr
