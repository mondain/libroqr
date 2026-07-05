add_library(roqr-gateway STATIC
  src/bridge.cpp
  src/rtmp_commands.cpp
  src/ingest.cpp
  src/egress.cpp
)

target_include_directories(roqr-gateway PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(roqr-gateway PUBLIC roqr-core roqr-quic roqr-rtmp)

target_compile_features(roqr-gateway PUBLIC cxx_std_20)
target_compile_options(roqr-gateway PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
