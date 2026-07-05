add_library(roqr-rtmp STATIC
  src/amf0.cpp
  src/handshake.cpp
  src/chunk_reader.cpp
  src/chunk_writer.cpp
)

target_include_directories(roqr-rtmp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(roqr-rtmp PUBLIC cxx_std_20)
target_compile_options(roqr-rtmp PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
