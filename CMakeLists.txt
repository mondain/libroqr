cmake_minimum_required(VERSION 3.24)

project(libroqr
  VERSION 0.1.0
  DESCRIPTION "RTMP over QUIC (RoQR) client library"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ROQR_BUILD_TESTS "Build unit tests" ON)

add_subdirectory(core)

if(ROQR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
