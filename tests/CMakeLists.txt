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

  if(ROQR_BUILD_TOOLS)
    set(ROQR_TEST_CERT_DIR "${CMAKE_CURRENT_BINARY_DIR}/testcerts")
    add_custom_command(
      OUTPUT "${ROQR_TEST_CERT_DIR}/cert.pem" "${ROQR_TEST_CERT_DIR}/key.pem"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${ROQR_TEST_CERT_DIR}"
      COMMAND openssl req -x509 -newkey ec
              -pkeyopt ec_paramgen_curve:prime256v1
              -keyout "${ROQR_TEST_CERT_DIR}/key.pem"
              -out "${ROQR_TEST_CERT_DIR}/cert.pem"
              -days 365 -nodes -subj "/CN=localhost"
      VERBATIM)
    add_custom_target(roqr-testcerts DEPENDS
      "${ROQR_TEST_CERT_DIR}/cert.pem" "${ROQR_TEST_CERT_DIR}/key.pem")

    add_executable(roqr-integration-tests
      integration/server_lifecycle_test.cpp
      integration/client_connect_test.cpp
      integration/stream_echo_test.cpp
      integration/datagram_test.cpp
      integration/flow_gating_test.cpp
      integration/relay_test.cpp
      integration/flow_reset_test.cpp
    )
    add_dependencies(roqr-integration-tests roqr-testcerts)
    target_compile_definitions(roqr-integration-tests PRIVATE
      ROQR_TEST_CERT_DIR="${ROQR_TEST_CERT_DIR}")
    target_link_libraries(roqr-integration-tests PRIVATE
      roqr-relayd-lib roqr-quic Catch2::Catch2WithMain)
    catch_discover_tests(roqr-integration-tests PROPERTIES TIMEOUT 60)
  endif()
endif()

if(ROQR_BUILD_RTMP)
  add_executable(roqr-rtmp-tests
    rtmp/amf0_encode_test.cpp
  )
  target_link_libraries(roqr-rtmp-tests PRIVATE roqr-rtmp Catch2::Catch2WithMain)
  catch_discover_tests(roqr-rtmp-tests PROPERTIES TIMEOUT 60)
endif()
