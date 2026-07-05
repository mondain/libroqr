cmake_minimum_required(VERSION 3.24)

project(libroqr
  VERSION 0.1.0
  DESCRIPTION "RTMP over QUIC (RoQR) client library"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(ROQR_BUILD_TESTS "Build unit tests" ON)
option(ROQR_BUILD_QUIC "Build the picoquic transport (needs picoquic)" OFF)
option(ROQR_BUILD_TOOLS "Build tools (roqr-relayd; needs ROQR_BUILD_QUIC)" OFF)
option(ROQR_BUILD_RTMP "Build the RTMP/AMF gateway module" ON)

add_subdirectory(core)

if(ROQR_BUILD_RTMP)
  add_subdirectory(rtmp)
endif()

if(ROQR_BUILD_QUIC)
  find_package(Picoquic REQUIRED)
  find_package(OpenSSL REQUIRED)
  add_subdirectory(quic)
endif()

if(ROQR_BUILD_TOOLS)
  if(NOT ROQR_BUILD_QUIC)
    message(FATAL_ERROR "ROQR_BUILD_TOOLS requires ROQR_BUILD_QUIC=ON")
  endif()
  add_subdirectory(tools/relayd)
endif()

if(ROQR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
