#include <catch2/catch_test_macros.hpp>

#include <cstring>

extern "C" {
#include "roqr/roqr.h"
}

TEST_CASE("ffi version matches the library version") {
    CHECK(std::strcmp(roqr_version(), "0.1.0") == 0);
}

TEST_CASE("ffi client create and destroy") {
    roqr_client* c = roqr_client_create();
    REQUIRE(c != nullptr);
    roqr_client_destroy(c);
    roqr_client_destroy(nullptr);  // must be a safe no-op
}

TEST_CASE("ffi error and delivery enum values are stable") {
    CHECK(ROQR_OK == 0);
    CHECK(ROQR_ERR_UNKNOWN_FLOW == 6);
    CHECK(ROQR_DELIVERY_AUTO == 2);
}
