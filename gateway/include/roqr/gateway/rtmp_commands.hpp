#pragma once

#include <optional>
#include <string>
#include <vector>

#include "roqr/rtmp/amf0.hpp"
#include "roqr/rtmp/message.hpp"

namespace roqr::gateway {

// A decoded RTMP AMF0 command: the command name, its transaction id, and
// every AMF0 value after the transaction id (command object, stream name,
// etc.).
struct Command {
    std::string name;
    double transaction_id = 0;
    std::vector<roqr::rtmp::Amf0Value> args;
};

std::optional<Command> parse_command(const roqr::rtmp::RtmpMessage& msg);

roqr::rtmp::RtmpMessage build_connect(double txn, const std::string& app,
                                      const std::string& tc_url);
roqr::rtmp::RtmpMessage build_create_stream(double txn);
roqr::rtmp::RtmpMessage build_publish(double txn,
                                      const std::string& stream_name);
roqr::rtmp::RtmpMessage build_play(double txn, const std::string& stream_name);
roqr::rtmp::RtmpMessage build_result_object(double txn,
                                            roqr::rtmp::Amf0Value props,
                                            roqr::rtmp::Amf0Value info);
roqr::rtmp::RtmpMessage build_result_stream_id(double txn, double stream_id);
roqr::rtmp::RtmpMessage build_on_status(const std::string& code,
                                        const std::string& description);

}  // namespace roqr::gateway
