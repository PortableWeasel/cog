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
