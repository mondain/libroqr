#include <catch2/catch_test_macros.hpp>

#include <string>

#include "roqr/relayd/server.hpp"

using namespace roqr::relayd;

namespace {
const std::string kCertDir = ROQR_TEST_CERT_DIR;
}

TEST_CASE("server starts on a loopback port and stops cleanly") {
    Server server;
    ServerOptions opts;
    opts.port = 45550;
    opts.cert_file = kCertDir + "/cert.pem";
    opts.key_file = kCertDir + "/key.pem";
    REQUIRE(server.start(opts));
    server.stop();
    // stop() is idempotent.
    server.stop();
}

TEST_CASE("server start fails with missing certs") {
    Server server;
    ServerOptions opts;
    opts.port = 45551;
    opts.cert_file = "/nonexistent/cert.pem";
    opts.key_file = "/nonexistent/key.pem";
    CHECK_FALSE(server.start(opts));
}
