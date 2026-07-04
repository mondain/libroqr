#include "roqr/error.hpp"

namespace roqr {

const char* to_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::NoError: return "NO_ERROR";
        case ErrorCode::GeneralError: return "GENERAL_ERROR";
        case ErrorCode::InternalError: return "INTERNAL_ERROR";
        case ErrorCode::FrameEncodingError: return "FRAME_ENCODING_ERROR";
        case ErrorCode::StreamCreationError: return "STREAM_CREATION_ERROR";
        case ErrorCode::FrameCancelled: return "FRAME_CANCELLED";
        case ErrorCode::UnknownFlowId: return "UNKNOWN_FLOW_ID";
        case ErrorCode::ExpectationUnmet: return "EXPECTATION_UNMET";
    }
    return "UNKNOWN";
}

}  // namespace roqr
