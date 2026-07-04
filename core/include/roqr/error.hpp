#pragma once

#include <cstdint>

namespace roqr {

// RoQR application error codes (draft Table 2).
enum class ErrorCode : uint64_t {
    NoError = 0x00,
    GeneralError = 0x01,
    InternalError = 0x02,
    FrameEncodingError = 0x03,
    StreamCreationError = 0x04,
    FrameCancelled = 0x05,
    UnknownFlowId = 0x06,
    ExpectationUnmet = 0x07,
};

// Registry name for the code, e.g. "FRAME_ENCODING_ERROR".
const char* to_string(ErrorCode code);

}  // namespace roqr
