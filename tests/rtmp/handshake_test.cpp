#include <catch2/catch_test_macros.hpp>

#include "roqr/rtmp/handshake.hpp"

using namespace roqr::rtmp;

TEST_CASE("initiator and responder complete a loopback handshake") {
    HandshakeInitiator client;
    HandshakeResponder server;

    std::vector<uint8_t> to_server = client.start();
    REQUIRE(to_server.size() == 1 + kHandshakePacketSize);
    CHECK(to_server[0] == kRtmpVersion);

    std::vector<uint8_t> to_client;
    REQUIRE(server.feed(to_server, to_client));
    REQUIRE(to_client.size() == 1 + 2 * kHandshakePacketSize);  // S0 S1 S2

    std::vector<uint8_t> c2;
    REQUIRE(client.feed(to_client, c2));
    REQUIRE(c2.size() == kHandshakePacketSize);
    CHECK(client.done());

    std::vector<uint8_t> none;
    REQUIRE(server.feed(c2, none));
    CHECK(none.empty());
    CHECK(server.done());
}

TEST_CASE("handshake survives byte-at-a-time delivery") {
    HandshakeInitiator client;
    HandshakeResponder server;

    std::vector<uint8_t> to_server = client.start();
    std::vector<uint8_t> to_client;
    for (uint8_t b : to_server) {
        REQUIRE(server.feed(std::span<const uint8_t>(&b, 1), to_client));
    }
    std::vector<uint8_t> c2;
    for (uint8_t b : to_client) {
        REQUIRE(client.feed(std::span<const uint8_t>(&b, 1), c2));
    }
    std::vector<uint8_t> none;
    for (uint8_t b : c2) {
        REQUIRE(server.feed(std::span<const uint8_t>(&b, 1), none));
    }
    CHECK(client.done());
    CHECK(server.done());
}

TEST_CASE("responder rejects a wrong version byte") {
    HandshakeResponder server;
    const uint8_t bad[] = {0x06};
    std::vector<uint8_t> out;
    CHECK_FALSE(server.feed(bad, out));
}

TEST_CASE("responder rejects a C2 that does not echo S1") {
    HandshakeInitiator client;
    HandshakeResponder server;
    std::vector<uint8_t> to_client;
    REQUIRE(server.feed(client.start(), to_client));

    std::vector<uint8_t> forged(kHandshakePacketSize, 0xEE);
    std::vector<uint8_t> none;
    CHECK_FALSE(server.feed(forged, none));
}
