#include "roqr/flow_table.hpp"

namespace roqr {

FlowTable::FlowTable(FlowTableLimits limits) : limits_(limits) {
    // Draft s5: Flow ID 0 is the default RTMP session flow.
    states_[0] = FlowState::Active;
}

bool FlowTable::activate(uint64_t flow_id) {
    auto it = states_.find(flow_id);
    if (it != states_.end() && it->second == FlowState::Retired) return false;
    states_[flow_id] = FlowState::Active;
    return true;
}

void FlowTable::retire(uint64_t flow_id) {
    states_[flow_id] = FlowState::Retired;
    take_buffered(flow_id);  // drop anything still queued for the flow
}

FlowState FlowTable::state(uint64_t flow_id) const {
    auto it = states_.find(flow_id);
    return it == states_.end() ? FlowState::Unknown : it->second;
}

FlowTable::BufferResult FlowTable::buffer_unknown(Frame frame) {
    if (unknown_.size() >= limits_.max_unknown_frames ||
        unknown_octets_ + frame.payload.size() > limits_.max_unknown_octets) {
        return BufferResult::LimitExceeded;
    }
    unknown_octets_ += frame.payload.size();
    unknown_.push_back(std::move(frame));
    return BufferResult::Buffered;
}

std::vector<Frame> FlowTable::take_buffered(uint64_t flow_id) {
    std::vector<Frame> out;
    for (auto it = unknown_.begin(); it != unknown_.end();) {
        if (it->flow_id == flow_id) {
            unknown_octets_ -= it->payload.size();
            out.push_back(std::move(*it));
            it = unknown_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

}  // namespace roqr
