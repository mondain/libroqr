add_library(roqr-quic STATIC
  src/context.cpp
)

target_include_directories(roqr-quic PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(roqr-quic
  PUBLIC roqr-core picoquic::picoquic-core
  PRIVATE OpenSSL::SSL OpenSSL::Crypto
)

target_compile_features(roqr-quic PUBLIC cxx_std_20)
target_compile_options(roqr-quic PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
