cmake_minimum_required(VERSION 3.24)

project(libroqr
  VERSION 0.1.0
  DESCRIPTION "RTMP over QUIC (RoQR) client library"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Collocate all built executables (roqr-relayd, roqr-ingest, roqr-egress,
# roqr-duplex, test binaries) in a single directory so tooling (e.g. the
# ffmpeg e2e test) can find the gateway trio via one bin dir.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(ROQR_BUILD_TESTS "Build unit tests" ON)
option(ROQR_BUILD_QUIC "Build the picoquic transport (needs picoquic)" OFF)
option(ROQR_BUILD_TOOLS "Build tools (roqr-relayd; needs ROQR_BUILD_QUIC)" OFF)
option(ROQR_BUILD_RTMP "Build the RTMP/AMF gateway module" ON)
option(ROQR_BUILD_EXAMPLES "Build gateway library and example apps" ON)
option(ROQR_BUILD_FFI "Build the C FFI shared library" ON)
option(ROQR_BUILD_JNI "Build JNI bindings (needs a JDK)" OFF)

# The SHARED roqr-ffi library links the static libraries (and picoquic), so
# they must be position-independent — but only when FFI is actually built.
# Setting this directory-scoped variable before add_subdirectory propagates
# it to every subproject (including the picoquic add_subdirectory), so the
# core-only build (no FFI) is unaffected.
if(ROQR_BUILD_FFI AND ROQR_BUILD_QUIC AND ROQR_BUILD_RTMP)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

add_subdirectory(core)

if(ROQR_BUILD_RTMP)
  add_subdirectory(rtmp)
endif()

# The gateway library is needed by both the example binaries and the
# relay's Media mode, so build it whenever its dependencies are present.
if(ROQR_BUILD_QUIC AND ROQR_BUILD_RTMP)
  add_subdirectory(gateway)
endif()

if(ROQR_BUILD_EXAMPLES)
  if(NOT ROQR_BUILD_QUIC OR NOT ROQR_BUILD_RTMP)
    message(FATAL_ERROR "ROQR_BUILD_EXAMPLES requires ROQR_BUILD_QUIC and ROQR_BUILD_RTMP")
  endif()
  if(ROQR_BUILD_TESTS AND NOT ROQR_BUILD_TOOLS)
    message(FATAL_ERROR "ROQR_BUILD_EXAMPLES with ROQR_BUILD_TESTS requires ROQR_BUILD_TOOLS")
  endif()
  add_subdirectory(examples)
endif()

if(ROQR_BUILD_QUIC)
  find_package(Picoquic REQUIRED)
  find_package(OpenSSL REQUIRED)
  add_subdirectory(quic)
endif()

if(ROQR_BUILD_TOOLS)
  if(NOT ROQR_BUILD_QUIC OR NOT ROQR_BUILD_RTMP)
    message(FATAL_ERROR "ROQR_BUILD_TOOLS requires ROQR_BUILD_QUIC and ROQR_BUILD_RTMP")
  endif()
  add_subdirectory(tools/relayd)
endif()

if(ROQR_BUILD_FFI)
  if(ROQR_BUILD_QUIC AND ROQR_BUILD_RTMP)
    add_subdirectory(ffi)
  else()
    message(STATUS "ROQR_BUILD_FFI needs QUIC and RTMP; skipping FFI in this config")
  endif()
endif()

if(ROQR_BUILD_JNI)
  if(NOT TARGET roqr-ffi)
    message(FATAL_ERROR "ROQR_BUILD_JNI requires the FFI library (ROQR_BUILD_FFI plus QUIC+RTMP)")
  endif()
  find_package(JNI)
  if(JNI_FOUND)
    add_subdirectory(jni)
  else()
    message(WARNING "JNI not found; skipping roqr-jni")
  endif()
endif()

if(ROQR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
