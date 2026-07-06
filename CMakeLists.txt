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

# Sanitizer selection (thread, address, undefined; empty = none). The flags are
# applied per-target at the end of this file to ROQR's own code ONLY — never the
# vendored picoquic, whose internal network-thread and PRNG data races we cannot
# fix and which would otherwise drown out races in our code. See the
# ROQR_SANITIZE block at the bottom.
set(ROQR_SANITIZE "" CACHE STRING
  "Enable a sanitizer: thread, address, or undefined (empty = none)")
if(ROQR_SANITIZE)
  message(STATUS "Building ROQR targets with -fsanitize=${ROQR_SANITIZE}")
endif()

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
  if(JNI_FOUND OR ANDROID)
    add_subdirectory(jni)
  else()
    message(WARNING "JNI not found; skipping roqr-jni")
  endif()
endif()

if(ROQR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Apply the selected sanitizer to ROQR's own targets only. Instrumenting the
# vendored picoquic (compiled from source under .deps) is deliberately avoided:
# its network-thread teardown and global-PRNG paths have internal data races we
# cannot fix from here, and instrumenting it drowns the signal in third-party
# noise. Because our code — including the on_message/on_closed callbacks that
# run on picoquic's network thread — stays instrumented, TSAN still detects real
# races in ROQR (both racing accesses land in our instrumented code); picoquic's
# uninstrumented internals simply aren't tracked. Fetched deps (Catch2, under
# the build tree) are skipped for the same reason.
if(ROQR_SANITIZE)
  function(_roqr_collect_targets dir out_var)
    get_property(_subs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    get_property(_acc DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_sub IN LISTS _subs)
      _roqr_collect_targets("${_sub}" _child)
      list(APPEND _acc ${_child})
    endforeach()
    set(${out_var} ${_acc} PARENT_SCOPE)
  endfunction()

  _roqr_collect_targets("${CMAKE_SOURCE_DIR}" _roqr_all_targets)
  set(_roqr_san_flags -fsanitize=${ROQR_SANITIZE} -fno-omit-frame-pointer -g)
  foreach(_tgt IN LISTS _roqr_all_targets)
    get_target_property(_type ${_tgt} TYPE)
    get_target_property(_src_dir ${_tgt} SOURCE_DIR)
    if(_type MATCHES "STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE"
       AND _src_dir MATCHES "^${CMAKE_SOURCE_DIR}"
       AND NOT _src_dir MATCHES "\\.deps"
       AND NOT _src_dir MATCHES "^${CMAKE_BINARY_DIR}")
      target_compile_options(${_tgt} PRIVATE ${_roqr_san_flags})
      target_link_options(${_tgt} PRIVATE -fsanitize=${ROQR_SANITIZE})
    endif()
  endforeach()
endif()
