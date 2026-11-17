add_executable(test_assertions test_assertions.c)
target_enable_warnings(test_assertions)
add_test(NAME build-test-assertions COMMAND test_assertions)
add_test(NAME build-test-assertion-failure
    COMMAND ${CMAKE_COMMAND} -DASSERTION_EXECUTABLE=$<TARGET_FILE:test_assertions>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assertion_failure.cmake)
add_test(NAME build-test-assertion-registration
    COMMAND ${CMAKE_COMMAND} -DBUILD_ROOT=${PROJECT_BINARY_DIR}
        -DASSERTION_MANIFEST=${PROJECT_BINARY_DIR}/test-assertions-$<CONFIG>.cmake
        -DCTEST_CONFIGURATION_TYPE=$<CONFIG>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assertion_registration.cmake)
set_tests_properties(build-test-assertions build-test-assertion-failure
    build-test-assertion-registration PROPERTIES LABELS "build-policy")

# A real nested registration reproduces the hidden -D wrapper false pass.
add_test(NAME build-test-wrapped-classification
    COMMAND ${CMAKE_COMMAND} -DCOG_SOURCE_ROOT=${PROJECT_SOURCE_DIR}
        "-DPROBE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/wrapped probes/$<CONFIG>"
        -DPROBE_GENERATOR=${CMAKE_GENERATOR}
        -DPROBE_C_COMPILER=${CMAKE_C_COMPILER} -DPROBE_CONFIG=$<CONFIG>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/wrapped_registration_failure.cmake)
set_tests_properties(build-test-wrapped-classification PROPERTIES LABELS "build-policy")

add_test(NAME build-curl-selection
    COMMAND ${CMAKE_COMMAND} -DCOG_SOURCE_ROOT=${PROJECT_SOURCE_DIR}
        "-DPROBE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/curl selection/$<CONFIG>"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/curl_selection.cmake)
set_tests_properties(build-curl-selection PROPERTIES LABELS "build-policy")
