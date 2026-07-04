#include <catch2/catch_test_macros.hpp>

#include "roqr/quic/delivery.hpp"

using namespace roqr::quic;

namespace {
constexpr size_t kMax = 1200;
}

TEST_CASE("Stream mode always resolves to stream") {
    CHECK(resolve_delivery(9, DeliveryMode::Stream, true, 100, kMax,
                           DatagramFallback::Stream) == ResolvedMode::Stream);
}

TEST_CASE("Auto sends non-media message types on stream") {
    // 20 = AMF0 Command, 18 = AMF0 Data, 4 = User Control: session
    // correctness traffic (draft s10) stays on streams even in Auto.
    for (uint8_t type : {1, 2, 3, 4, 5, 6, 15, 17, 18, 19, 20}) {
        CHECK(resolve_delivery(type, DeliveryMode::Auto, true, 100, kMax,
                               DatagramFallback::Stream) ==
              ResolvedMode::Stream);
    }
}

TEST_CASE("Auto sends fitting media in datagrams when negotiated") {
    for (uint8_t type : {8, 9, 22}) {  // Audio, Video, Aggregate
        CHECK(resolve_delivery(type, DeliveryMode::Auto, true, 100, kMax,
                               DatagramFallback::Stream) ==
              ResolvedMode::Datagram);
    }
}

TEST_CASE("Auto falls back to stream when not negotiated or too large") {
    CHECK(resolve_delivery(9, DeliveryMode::Auto, false, 100, kMax,
                           DatagramFallback::Stream) == ResolvedMode::Stream);
    CHECK(resolve_delivery(9, DeliveryMode::Auto, true, kMax + 1, kMax,
                           DatagramFallback::Stream) == ResolvedMode::Stream);
}

TEST_CASE("Datagram mode honors the fallback policy") {
    CHECK(resolve_delivery(9, DeliveryMode::Datagram, true, 100, kMax,
                           DatagramFallback::Stream) ==
          ResolvedMode::Datagram);
    CHECK(resolve_delivery(9, DeliveryMode::Datagram, true, kMax + 1, kMax,
                           DatagramFallback::Stream) == ResolvedMode::Stream);
    CHECK(resolve_delivery(9, DeliveryMode::Datagram, true, kMax + 1, kMax,
                           DatagramFallback::Drop) == ResolvedMode::Dropped);
    CHECK(resolve_delivery(9, DeliveryMode::Datagram, false, 100, kMax,
                           DatagramFallback::Drop) == ResolvedMode::Dropped);
    CHECK(resolve_delivery(9, DeliveryMode::Datagram, false, 100, kMax,
                           DatagramFallback::Stream) == ResolvedMode::Stream);
}
