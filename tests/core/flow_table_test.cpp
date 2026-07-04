#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "roqr/error.hpp"
#include "roqr/flow_table.hpp"

using namespace roqr;

TEST_CASE("error codes match draft Table 2") {
    CHECK(static_cast<uint64_t>(ErrorCode::NoError) == 0x00);
    CHECK(static_cast<uint64_t>(ErrorCode::GeneralError) == 0x01);
    CHECK(static_cast<uint64_t>(ErrorCode::InternalError) == 0x02);
    CHECK(static_cast<uint64_t>(ErrorCode::FrameEncodingError) == 0x03);
    CHECK(static_cast<uint64_t>(ErrorCode::StreamCreationError) == 0x04);
    CHECK(static_cast<uint64_t>(ErrorCode::FrameCancelled) == 0x05);
    CHECK(static_cast<uint64_t>(ErrorCode::UnknownFlowId) == 0x06);
    CHECK(static_cast<uint64_t>(ErrorCode::ExpectationUnmet) == 0x07);

    CHECK(std::strcmp(to_string(ErrorCode::FrameEncodingError),
                      "FRAME_ENCODING_ERROR") == 0);
    CHECK(std::strcmp(to_string(ErrorCode::UnknownFlowId),
                      "UNKNOWN_FLOW_ID") == 0);
}

TEST_CASE("flow 0 is the default session flow and starts Active") {
    FlowTable table;
    CHECK(table.state(0) == FlowState::Active);
}

TEST_CASE("unbound flows are Unknown until activated") {
    FlowTable table;
    CHECK(table.state(7) == FlowState::Unknown);
    CHECK(table.activate(7));
    CHECK(table.state(7) == FlowState::Active);
}

TEST_CASE("retired flows cannot be reactivated") {
    FlowTable table;
    REQUIRE(table.activate(7));
    table.retire(7);
    CHECK(table.state(7) == FlowState::Retired);
    CHECK_FALSE(table.activate(7));
    CHECK(table.state(7) == FlowState::Retired);
}

TEST_CASE("activating an already-active flow is idempotent") {
    FlowTable table;
    REQUIRE(table.activate(7));
    CHECK(table.activate(7));
    CHECK(table.state(7) == FlowState::Active);
}
