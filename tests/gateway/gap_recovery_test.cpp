#include <catch2/catch_test_macros.hpp>

#include "roqr/gateway/gap.hpp"

using namespace roqr::gateway;
using roqr::rtmp::MediaClass;

TEST_CASE("continuous stream delivers everything") {
    GapTracker g;
    CHECK(g.accept(0, MediaClass::SequenceHeader));
    CHECK(g.accept(40, MediaClass::Keyframe));
    CHECK(g.accept(80, MediaClass::Coded));
    CHECK(g.accept(120, MediaClass::Coded));
}

TEST_CASE("a forward jump drops coded frames until a keyframe") {
    GapTracker g;
    CHECK(g.accept(0, MediaClass::Keyframe));
    CHECK(g.accept(40, MediaClass::Coded));
    // Jump well past the threshold: discontinuity.
    CHECK_FALSE(g.accept(40 + GapTracker::kJumpThreshold + 1,
                         MediaClass::Coded));
    // Still dropping non-recovery frames.
    CHECK_FALSE(g.accept(10000, MediaClass::Coded));
    // A keyframe recovers the timeline.
    CHECK(g.accept(10040, MediaClass::Keyframe));
    CHECK(g.accept(10080, MediaClass::Coded));
}

TEST_CASE("a timestamp regression triggers recovery") {
    GapTracker g;
    CHECK(g.accept(1000, MediaClass::Keyframe));
    CHECK(g.accept(1040, MediaClass::Coded));
    CHECK_FALSE(g.accept(500, MediaClass::Coded));  // regression
    CHECK(g.accept(1080, MediaClass::SequenceHeader));  // recovers
    CHECK(g.accept(1120, MediaClass::Coded));
}
