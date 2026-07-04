#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "roqr/quic/client.hpp"
#include "roqr/relayd/server.hpp"

using namespace std::chrono_literals;

namespace {
const std::string kCertDir = ROQR_TEST_CERT_DIR;

roqr::relayd::ServerOptions server_opts(uint16_t port) {
    roqr::relayd::ServerOptions o;
    o.port = port;
    o.cert_file = kCertDir + "/cert.pem";
    o.key_file = kCertDir + "/key.pem";
    return o;
}
}  // namespace

TEST_CASE("client completes the roqr handshake against the relay") {
    roqr::relayd::Server server;
    REQUIRE(server.start(server_opts(45552)));

    roqr::quic::Client client;
    REQUIRE(client.connect("127.0.0.1", 45552));
    CHECK(client.wait_connected(5s));

    client.close();
    CHECK(client.wait_closed(5s));
    server.stop();
}

TEST_CASE("wait_connected times out when no server is listening") {
    roqr::quic::Client client;
    REQUIRE(client.connect("127.0.0.1", 45553));  // nothing listening
    CHECK_FALSE(client.wait_connected(2s));
}

TEST_CASE("connect refuses a second call on the same client") {
    roqr::relayd::Server server;
    REQUIRE(server.start(server_opts(45561)));

    roqr::quic::Client client;
    REQUIRE(client.connect("127.0.0.1", 45561));
    CHECK_FALSE(client.connect("127.0.0.1", 45561));

    client.close();
    client.wait_closed(5s);
    server.stop();
}
