cmake_minimum_required(VERSION 3.16)
# WILL_FAIL does not invert abort signals. Inspect a bounded child explicitly,
# and require both reachability and the assertion diagnostic (not a launch error).
execute_process(COMMAND "${ASSERTION_EXECUTABLE}" --fail
    RESULT_VARIABLE outcome OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
if("${outcome}" STREQUAL "0" OR
   NOT output MATCHES "COG_TEST_ASSERTION_REACHED" OR
   NOT error MATCHES "COG_TEST_ASSERTION_FAILURE")
    message(FATAL_ERROR "Intentional assertion failure was not observed: ${outcome}\n${output}\n${error}")
endif()
