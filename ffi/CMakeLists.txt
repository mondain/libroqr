add_library(roqr-ffi SHARED
  src/roqr_ffi.cpp
  src/roqr_rtmp_ffi.cpp
)

target_include_directories(roqr-ffi PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(roqr-ffi PRIVATE roqr-core roqr-quic roqr-gateway)

target_compile_features(roqr-ffi PRIVATE cxx_std_20)
target_compile_options(roqr-ffi PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)

set_target_properties(roqr-ffi PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION 0)

install(TARGETS roqr-ffi LIBRARY DESTINATION lib)
install(DIRECTORY include/roqr DESTINATION include)
