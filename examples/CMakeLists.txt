add_executable(roqr-ingest ingest_main.cpp)
target_link_libraries(roqr-ingest PRIVATE roqr-gateway)

add_executable(roqr-egress egress_main.cpp)
target_link_libraries(roqr-egress PRIVATE roqr-gateway)

add_executable(roqr-duplex duplex_main.cpp)
target_link_libraries(roqr-duplex PRIVATE roqr-gateway)

foreach(t roqr-ingest roqr-egress roqr-duplex)
  target_compile_options(${t} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
endforeach()
