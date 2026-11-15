# This subdirectory runs before normal library/test directories. It proves a
# nonconventional owning target can call the public test helper immediately.
add_executable(custom_assertion_probe ../test_assertions.c)
cog_enable_test_assertions(custom_assertion_probe)
target_enable_warnings(custom_assertion_probe)
add_test(NAME build-test-explicit-optin COMMAND custom_assertion_probe)
add_test(NAME build-test-explicit-optin-failure
    COMMAND ${CMAKE_COMMAND} -DASSERTION_EXECUTABLE=$<TARGET_FILE:custom_assertion_probe>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/../assertion_failure.cmake)
set_tests_properties(build-test-explicit-optin build-test-explicit-optin-failure
    PROPERTIES LABELS "build-policy")
