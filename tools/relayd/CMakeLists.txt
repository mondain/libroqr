add_library(roqr-relayd-lib STATIC
  src/server.cpp
  src/media_router.cpp
)

target_include_directories(roqr-relayd-lib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(roqr-relayd-lib PUBLIC roqr-quic)

target_compile_options(roqr-relayd-lib PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)

add_executable(roqr-relayd src/main.cpp)
target_link_libraries(roqr-relayd PRIVATE roqr-relayd-lib)

target_compile_options(roqr-relayd PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
