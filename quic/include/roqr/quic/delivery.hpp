#pragma once

#include <cstddef>
#include <cstdint>

namespace roqr::quic {

enum class DeliveryMode { Stream, Datagram, Auto };

// What to do when a Datagram-mode frame cannot go in a datagram
// (extension not negotiated, or frame exceeds the max datagram size):
// send it on the stream instead (default) or drop it (draft s7.5).
enum class DatagramFallback { Stream, Drop };

enum class ResolvedMode { Stream, Datagram, Dropped };

// Applies draft s7.5/s10 policy. Auto keeps session-correctness traffic
// (commands, control, data/metadata, shared objects) on streams; audio (8),
// video (9), and aggregate (22) messages use datagrams when negotiated and
// the encoded frame fits.
ResolvedMode resolve_delivery(uint8_t message_type, DeliveryMode requested,
                              bool datagram_negotiated, size_t encoded_size,
                              size_t max_datagram_size,
                              DatagramFallback fallback);

}  // namespace roqr::quic
