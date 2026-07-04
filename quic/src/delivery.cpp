#include "roqr/quic/delivery.hpp"

namespace roqr::quic {

namespace {
bool is_media_type(uint8_t message_type) {
    return message_type == 8 || message_type == 9 || message_type == 22;
}
}  // namespace

ResolvedMode resolve_delivery(uint8_t message_type, DeliveryMode requested,
                              bool datagram_negotiated, size_t encoded_size,
                              size_t max_datagram_size,
                              DatagramFallback fallback) {
    if (requested == DeliveryMode::Stream) return ResolvedMode::Stream;

    const bool fits = datagram_negotiated && encoded_size <= max_datagram_size;

    if (requested == DeliveryMode::Datagram) {
        if (fits) return ResolvedMode::Datagram;
        return fallback == DatagramFallback::Stream ? ResolvedMode::Stream
                                                    : ResolvedMode::Dropped;
    }

    // Auto: media only, and only when it fits; everything else on stream.
    if (is_media_type(message_type) && fits) return ResolvedMode::Datagram;
    return ResolvedMode::Stream;
}

}  // namespace roqr::quic
