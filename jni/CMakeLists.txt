find_package(Java REQUIRED COMPONENTS Development)
include(UseJava)

add_library(roqr-jni SHARED
  src/roqr_jni.cpp
)
target_include_directories(roqr-jni PRIVATE ${JNI_INCLUDE_DIRS})
target_link_libraries(roqr-jni PRIVATE roqr-ffi ${JNI_LIBRARIES})
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
  OUTPUT_NAME roqr)

# Expose the jar path and the jni lib dir to the test subdirectory.
get_target_property(ROQR_JAR_FILE roqr-jar JAR_FILE)
set(ROQR_JAR_FILE "${ROQR_JAR_FILE}" CACHE INTERNAL "roqr jar path")
