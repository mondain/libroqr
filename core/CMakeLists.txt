add_library(roqr-core STATIC
  src/error.cpp
  src/flow_table.cpp
  src/frame.cpp
  src/varint.cpp
  src/version.cpp
)

target_include_directories(roqr-core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(roqr-core PUBLIC cxx_std_20)

target_compile_options(roqr-core PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
