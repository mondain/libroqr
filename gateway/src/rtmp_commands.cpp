#include "roqr/gateway/rtmp_commands.hpp"

namespace roqr::gateway {

using roqr::rtmp::Amf0Value;
using roqr::rtmp::RtmpMessage;

namespace {

RtmpMessage command_message(const std::vector<Amf0Value>& values,
                            uint32_t message_stream_id) {
    RtmpMessage m;
    m.chunk_stream_id = 3;
    m.type = roqr::rtmp::kTypeCommandAmf0;
    m.message_stream_id = message_stream_id;
    for (const auto& v : values) roqr::rtmp::amf0_encode(v, m.payload);
    return m;
}

Amf0Value status_info(const std::string& code, const std::string& desc) {
    Amf0Value info = Amf0Value::object();
    info.set("level", Amf0Value::string("status"))
        .set("code", Amf0Value::string(code))
        .set("description", Amf0Value::string(desc));
    return info;
}

}  // namespace

std::optional<Command> parse_command(const RtmpMessage& msg) {
    if (msg.type != roqr::rtmp::kTypeCommandAmf0) return std::nullopt;
    auto values = roqr::rtmp::amf0_decode_all(msg.payload);
    if (!values || values->size() < 2) return std::nullopt;
    if ((*values)[0].type() != Amf0Value::Type::String) return std::nullopt;
    if ((*values)[1].type() != Amf0Value::Type::Number) return std::nullopt;

    Command cmd;
    cmd.name = (*values)[0].as_string();
    cmd.transaction_id = (*values)[1].as_number();
    cmd.args.assign(values->begin() + 2, values->end());
    return cmd;
}

RtmpMessage build_connect(double txn, const std::string& app,
                          const std::string& tc_url) {
    Amf0Value obj = Amf0Value::object();
    obj.set("app", Amf0Value::string(app))
        .set("tcUrl", Amf0Value::string(tc_url))
        .set("type", Amf0Value::string("nonprivate"));
    return command_message(
        {Amf0Value::string("connect"), Amf0Value::number(txn), obj}, 0);
}

RtmpMessage build_create_stream(double txn) {
    return command_message({Amf0Value::string("createStream"),
                            Amf0Value::number(txn), Amf0Value::null()},
                           0);
}

RtmpMessage build_publish(double txn, const std::string& stream_name) {
    return command_message(
        {Amf0Value::string("publish"), Amf0Value::number(txn),
         Amf0Value::null(), Amf0Value::string(stream_name),
         Amf0Value::string("live")},
        1);
}

RtmpMessage build_play(double txn, const std::string& stream_name) {
    return command_message(
        {Amf0Value::string("play"), Amf0Value::number(txn), Amf0Value::null(),
         Amf0Value::string(stream_name)},
        1);
}

RtmpMessage build_result_object(double txn, Amf0Value props, Amf0Value info) {
    return command_message({Amf0Value::string("_result"),
                            Amf0Value::number(txn), std::move(props),
                            std::move(info)},
                           0);
}

RtmpMessage build_result_stream_id(double txn, double stream_id) {
    return command_message({Amf0Value::string("_result"),
                            Amf0Value::number(txn), Amf0Value::null(),
                            Amf0Value::number(stream_id)},
                           0);
}

RtmpMessage build_on_status(const std::string& code,
                            const std::string& description) {
    return command_message(
        {Amf0Value::string("onStatus"), Amf0Value::number(0),
         Amf0Value::null(), status_info(code, description)},
        1);
}

}  // namespace roqr::gateway
