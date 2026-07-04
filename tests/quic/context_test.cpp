#include <catch2/catch_test_macros.hpp>

#include "roqr/quic/context.hpp"

using namespace roqr::quic;

TEST_CASE("client context creates and destroys cleanly") {
    auto ctx = QuicContext::create_client("roqr", true);
    REQUIRE(ctx != nullptr);
    CHECK(ctx->get() != nullptr);
}

TEST_CASE("server context requires readable cert and key") {
    auto ctx = QuicContext::create_server(
        "roqr", "/nonexistent/cert.pem", "/nonexistent/key.pem", nullptr,
        nullptr);
    // picoquic_create fails (returns null) when the cert cannot be loaded.
    CHECK(ctx == nullptr);
}
