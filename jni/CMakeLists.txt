find_package(Java REQUIRED COMPONENTS Development)
include(UseJava)

add_library(roqr-jni SHARED
  src/roqr_jni.cpp
)
if(NOT ANDROID)
  target_include_directories(roqr-jni PRIVATE ${JNI_INCLUDE_DIRS})
  target_link_libraries(roqr-jni PRIVATE roqr-ffi ${JNI_LIBRARIES})
else()
  target_link_libraries(roqr-jni PRIVATE roqr-ffi)
endif()
target_compile_features(roqr-jni PRIVATE cxx_std_20)
target_compile_options(roqr-jni PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)

add_jar(roqr-jar
  SOURCES
    java/org/red5/roqr/RoqrNative.java
    java/org/red5/roqr/package-info.java
    java/org/red5/roqr/DeliveryMode.java
    java/org/red5/roqr/Frame.java
    java/org/red5/roqr/MessageListener.java
    java/org/red5/roqr/RoqrClient.java
    java/org/red5/roqr/IngestGateway.java
    java/org/red5/roqr/EgressGateway.java
  OUTPUT_NAME roqr)

add_jar(roqr-samples
  SOURCES
    ${CMAKE_SOURCE_DIR}/examples/java/PublishSample.java
    ${CMAKE_SOURCE_DIR}/examples/java/PlaySample.java
  INCLUDE_JARS roqr-jar
  OUTPUT_NAME roqr-samples)

# Expose the jar path and the jni lib dir to the test subdirectory.
get_target_property(ROQR_JAR_FILE roqr-jar JAR_FILE)
set(ROQR_JAR_FILE "${ROQR_JAR_FILE}" CACHE INTERNAL "roqr jar path")
