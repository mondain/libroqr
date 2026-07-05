#include <catch2/catch_test_macros.hpp>

#include "roqr/gateway/rtmp_commands.hpp"

using namespace roqr::gateway;
using roqr::rtmp::Amf0Value;

TEST_CASE("build_connect round-trips through parse_command") {
    const auto msg = build_connect(1, "live", "rtmp://h/live");
    CHECK(msg.type == 20);
    CHECK(msg.chunk_stream_id == 3);

    auto cmd = parse_command(msg);
    REQUIRE(cmd.has_value());
    CHECK(cmd->name == "connect");
    CHECK(cmd->transaction_id == 1);
    REQUIRE(cmd->args.size() >= 1);
    REQUIRE(cmd->args[0].type() == Amf0Value::Type::Object);
    const Amf0Value* app = cmd->args[0].find("app");
    REQUIRE(app != nullptr);
    CHECK(app->as_string() == "live");
}

TEST_CASE("build_publish and build_play carry the stream name as arg") {
    auto pub = parse_command(build_publish(3, "cam"));
    REQUIRE(pub.has_value());
    CHECK(pub->name == "publish");
    // args: [null command object, "cam", ...]; find the first string arg.
    bool found = false;
    for (const auto& a : pub->args) {
        if (a.type() == Amf0Value::Type::String && a.as_string() == "cam") {
            found = true;
        }
    }
    CHECK(found);

    auto play = parse_command(build_play(4, "cam"));
    REQUIRE(play.has_value());
    CHECK(play->name == "play");
    CHECK(build_play(4, "cam").message_stream_id == 1);
}

TEST_CASE("build_create_stream and its result carry a stream id") {
    CHECK(parse_command(build_create_stream(2))->name == "createStream");
    auto res = parse_command(build_result_stream_id(2, 1));
    REQUIRE(res.has_value());
    CHECK(res->name == "_result");
    CHECK(res->transaction_id == 2);
    // The stream id is the value after null.
    bool has_one = false;
    for (const auto& a : res->args) {
        if (a.type() == Amf0Value::Type::Number && a.as_number() == 1.0) {
            has_one = true;
        }
    }
    CHECK(has_one);
}

TEST_CASE("build_on_status carries level status and the code") {
    auto st = parse_command(build_on_status("NetStream.Play.Start", "ok"));
    REQUIRE(st.has_value());
    CHECK(st->name == "onStatus");
    const Amf0Value* info = nullptr;
    for (const auto& a : st->args) {
        if (a.type() == Amf0Value::Type::Object && a.find("code") != nullptr) {
            info = &a;
        }
    }
    REQUIRE(info != nullptr);
    CHECK(info->find("code")->as_string() == "NetStream.Play.Start");
    CHECK(info->find("level")->as_string() == "status");
}

TEST_CASE("parse_command rejects non-command messages") {
    roqr::rtmp::RtmpMessage video;
    video.type = 9;
    video.payload = {0x17};
    CHECK_FALSE(parse_command(video).has_value());

    roqr::rtmp::RtmpMessage empty_cmd;
    empty_cmd.type = 20;  // no payload -> cannot decode name
    CHECK_FALSE(parse_command(empty_cmd).has_value());
}
