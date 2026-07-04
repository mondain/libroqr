#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "roqr/version.hpp"

TEST_CASE("version string matches project version") {
    CHECK(std::strcmp(roqr::version(), "0.1.0") == 0);
}
