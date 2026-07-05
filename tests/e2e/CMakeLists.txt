# The e2e script needs the three binaries and the cert fixture. It is a
# plain CTest test that self-skips without ffmpeg.
add_test(
  NAME roqr-ffmpeg-e2e-h264
  COMMAND ${CMAKE_COMMAND} -E env
    ${CMAKE_CURRENT_SOURCE_DIR}/run_ffmpeg_e2e.sh
    $<TARGET_FILE_DIR:roqr-relayd> ${ROQR_TEST_CERT_DIR} h264)
set_tests_properties(roqr-ffmpeg-e2e-h264 PROPERTIES TIMEOUT 60)

add_test(
  NAME roqr-ffmpeg-e2e-hevc
  COMMAND ${CMAKE_COMMAND} -E env
    ${CMAKE_CURRENT_SOURCE_DIR}/run_ffmpeg_e2e.sh
    $<TARGET_FILE_DIR:roqr-relayd> ${ROQR_TEST_CERT_DIR} hevc)
set_tests_properties(roqr-ffmpeg-e2e-hevc PROPERTIES TIMEOUT 60)
