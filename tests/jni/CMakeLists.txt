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

if(TARGET roqr-relayd)
  add_jar(roqr-jni-client-test
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/RoqrClientTest.java
    INCLUDE_JARS roqr-jar)
  get_target_property(ROQR_CLIENT_TEST_JAR roqr-jni-client-test JAR_FILE)
  add_dependencies(roqr-jni-client-test roqr-testcerts)

  add_test(
    NAME roqr-jni-client
    COMMAND ${Java_JAVA_EXECUTABLE}
      -Djava.library.path=$<TARGET_FILE_DIR:roqr-jni>
      -cp "${ROQR_JAR_FILE}:${ROQR_CLIENT_TEST_JAR}"
      RoqrClientTest $<TARGET_FILE:roqr-relayd> ${ROQR_TEST_CERT_DIR})
  set_tests_properties(roqr-jni-client PROPERTIES TIMEOUT 60)

  add_jar(roqr-jni-gateway-test
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/RoqrGatewayTest.java
    INCLUDE_JARS roqr-jar)
  get_target_property(ROQR_GATEWAY_TEST_JAR roqr-jni-gateway-test JAR_FILE)
  add_dependencies(roqr-jni-gateway-test roqr-testcerts)

  add_test(
    NAME roqr-jni-gateway
    COMMAND ${Java_JAVA_EXECUTABLE}
      -Djava.library.path=$<TARGET_FILE_DIR:roqr-jni>
      -cp "${ROQR_JAR_FILE}:${ROQR_GATEWAY_TEST_JAR}"
      RoqrGatewayTest $<TARGET_FILE:roqr-relayd> ${ROQR_TEST_CERT_DIR})
  set_tests_properties(roqr-jni-gateway PROPERTIES TIMEOUT 60)
endif()
