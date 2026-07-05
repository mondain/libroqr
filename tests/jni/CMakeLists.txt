find_package(Java REQUIRED COMPONENTS Runtime)
include(UseJava)

# Compile the Java test against the roqr jar.
add_jar(roqr-jni-smoke-test
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/RoqrSmokeTest.java
  INCLUDE_JARS roqr-jar)
get_target_property(ROQR_SMOKE_JAR roqr-jni-smoke-test JAR_FILE)

add_test(
  NAME roqr-jni-smoke
  COMMAND ${Java_JAVA_EXECUTABLE}
    -Djava.library.path=$<TARGET_FILE_DIR:roqr-jni>
    -cp "${ROQR_JAR_FILE}:${ROQR_SMOKE_JAR}"
    RoqrSmokeTest)
set_tests_properties(roqr-jni-smoke PROPERTIES TIMEOUT 60)
