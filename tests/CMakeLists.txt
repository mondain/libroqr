include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
)
FetchContent_MakeAvailable(Catch2)

add_executable(roqr-core-tests
  core/version_test.cpp
  core/varint_test.cpp
  core/frame_test.cpp
)

target_link_libraries(roqr-core-tests PRIVATE roqr-core Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(roqr-core-tests)
