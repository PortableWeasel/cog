cmake_minimum_required(VERSION 3.16)
execute_process(COMMAND "${EXECUTABLE}" RESULT_VARIABLE outcome
    OUTPUT_VARIABLE output TIMEOUT 5)
message("${output}")
if(NOT "${outcome}" STREQUAL "0")
    message(FATAL_ERROR "Probe execution failed: ${outcome}")
endif()
