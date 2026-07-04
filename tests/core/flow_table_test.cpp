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

namespace {

Frame frame_for_flow(uint64_t flow_id, size_t payload_size) {
    Frame f;
    f.flow_id = flow_id;
    f.message_type = 9;
    f.payload.assign(payload_size, 0xAB);
    return f;
}

}  // namespace

TEST_CASE("unknown-flow frames buffer and drain in order") {
    FlowTable table;
    CHECK(table.buffer_unknown(frame_for_flow(5, 10)) ==
          FlowTable::BufferResult::Buffered);
    Frame second = frame_for_flow(5, 20);
    second.timestamp = 99;
    CHECK(table.buffer_unknown(second) == FlowTable::BufferResult::Buffered);

    auto drained = table.take_buffered(5);
    REQUIRE(drained.size() == 2);
    CHECK(drained[0].payload.size() == 10);
    CHECK(drained[1].timestamp == 99);
    CHECK(table.take_buffered(5).empty());
}

TEST_CASE("take_buffered only drains the requested flow") {
    FlowTable table;
    REQUIRE(table.buffer_unknown(frame_for_flow(5, 10)) ==
            FlowTable::BufferResult::Buffered);
    REQUIRE(table.buffer_unknown(frame_for_flow(6, 10)) ==
            FlowTable::BufferResult::Buffered);

    CHECK(table.take_buffered(5).size() == 1);
    CHECK(table.take_buffered(6).size() == 1);
}

TEST_CASE("frame-count limit is enforced") {
    FlowTable table(FlowTableLimits{.max_unknown_frames = 2,
                                    .max_unknown_octets = 1024});
    CHECK(table.buffer_unknown(frame_for_flow(5, 1)) ==
          FlowTable::BufferResult::Buffered);
    CHECK(table.buffer_unknown(frame_for_flow(5, 1)) ==
          FlowTable::BufferResult::Buffered);
    CHECK(table.buffer_unknown(frame_for_flow(5, 1)) ==
          FlowTable::BufferResult::LimitExceeded);
}

TEST_CASE("octet limit is enforced and released on drain") {
    FlowTable table(FlowTableLimits{.max_unknown_frames = 100,
                                    .max_unknown_octets = 25});
    CHECK(table.buffer_unknown(frame_for_flow(5, 20)) ==
          FlowTable::BufferResult::Buffered);
    CHECK(table.buffer_unknown(frame_for_flow(6, 20)) ==
          FlowTable::BufferResult::LimitExceeded);

    CHECK(table.take_buffered(5).size() == 1);
    CHECK(table.buffer_unknown(frame_for_flow(6, 20)) ==
          FlowTable::BufferResult::Buffered);
}

TEST_CASE("retiring a flow drops its buffered frames") {
    FlowTable table(FlowTableLimits{.max_unknown_frames = 100,
                                    .max_unknown_octets = 25});
    REQUIRE(table.buffer_unknown(frame_for_flow(5, 20)) ==
            FlowTable::BufferResult::Buffered);
    table.retire(5);
    CHECK(table.take_buffered(5).empty());
    // The octet budget was released by retire().
    CHECK(table.buffer_unknown(frame_for_flow(6, 20)) ==
          FlowTable::BufferResult::Buffered);
}
