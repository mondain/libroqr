#include <catch2/catch_test_macros.hpp>

#include "roqr/gateway/bridge.hpp"

using namespace roqr::gateway;

namespace {
roqr::rtmp::RtmpMessage rtmp_video(uint32_t ts) {
    roqr::rtmp::RtmpMessage m;
    m.timestamp = ts;
    m.type = 9;
    m.message_stream_id = 1;
    m.chunk_stream_id = 6;
    m.payload = {0x17, 0x01, 0xAA};
    return m;
}
}  // namespace

TEST_CASE("to_frame widens all metadata onto the flow") {
    const auto m = rtmp_video(1000);
    const roqr::Frame f = to_frame(m, 3);
    CHECK(f.flow_id == 3);
    CHECK(f.timestamp == 1000);
    CHECK(f.message_type == 9);
    CHECK(f.message_stream_id == 1);
    CHECK(f.chunk_stream_id == 6);
    CHECK(f.payload == m.payload);
}

TEST_CASE("to_rtmp narrows a well-formed frame") {
    roqr::Frame f = to_frame(rtmp_video(2000), 0);
    roqr::rtmp::RtmpMessage out;
    REQUIRE(to_rtmp(f, out));
    CHECK(out == rtmp_video(2000));
}

TEST_CASE("round-trip preserves every rtmp message") {
    const auto m = rtmp_video(0x00FF00FF);
    roqr::rtmp::RtmpMessage back;
    REQUIRE(to_rtmp(to_frame(m, 7), back));
    CHECK(back == m);
}

TEST_CASE("to_rtmp rejects fields that overflow RTMP widths") {
    roqr::Frame f = to_frame(rtmp_video(1), 0);
    roqr::rtmp::RtmpMessage out;

    f.timestamp = uint64_t{0xFFFFFFFF} + 1;
    CHECK_FALSE(to_rtmp(f, out));

    f = to_frame(rtmp_video(1), 0);
    f.message_stream_id = uint64_t{0xFFFFFFFF} + 1;
    CHECK_FALSE(to_rtmp(f, out));

    f = to_frame(rtmp_video(1), 0);
    f.chunk_stream_id = uint64_t{0xFFFFFFFF} + 1;
    CHECK_FALSE(to_rtmp(f, out));

    f = to_frame(rtmp_video(1), 0);
    f.message_type = 9;  // valid; message_type is already uint8 in Frame
    CHECK(to_rtmp(f, out));  // sanity: this one passes
}
