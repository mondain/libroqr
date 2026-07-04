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
  core/flow_table_test.cpp
)

target_link_libraries(roqr-core-tests PRIVATE roqr-core Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(roqr-core-tests)

if(ROQR_BUILD_QUIC)
  add_executable(roqr-quic-tests
    quic/context_test.cpp
    quic/delivery_test.cpp
    quic/outbound_queue_test.cpp
  )
  target_link_libraries(roqr-quic-tests PRIVATE roqr-quic Catch2::Catch2WithMain)
  catch_discover_tests(roqr-quic-tests)
endif()
