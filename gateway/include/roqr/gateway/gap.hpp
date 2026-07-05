#pragma once

#include <cstdint>

#include "roqr/rtmp/classify.hpp"

namespace roqr::gateway {

// Tracks continuity for a single video timeline (draft s8). Feed each video
// frame's timestamp and its MediaClass; returns whether to deliver it. On a
// suspected gap (timestamp regression or a jump past kJumpThreshold) it
// drops non-recovery frames until the next keyframe or sequence header.
class GapTracker {
public:
    static constexpr uint32_t kJumpThreshold = 5000;  // ms

    bool accept(uint32_t timestamp, roqr::rtmp::MediaClass cls) {
        const bool recover = cls == roqr::rtmp::MediaClass::Keyframe ||
                             cls == roqr::rtmp::MediaClass::SequenceHeader;
        if (have_) {
            const bool regressed = timestamp < last_ts_;
            const bool jumped =
                timestamp >
                static_cast<uint64_t>(last_ts_) + kJumpThreshold;
            if (regressed || jumped) discontinuous_ = true;
        }
        have_ = true;
        last_ts_ = timestamp;
        if (discontinuous_ && !recover) return false;
        if (recover) discontinuous_ = false;
        return true;
    }

private:
    bool discontinuous_ = false;
    bool have_ = false;
    uint32_t last_ts_ = 0;
};

}  // namespace roqr::gateway
