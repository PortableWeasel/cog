foreach(required COG_SOURCE_DIR BINARY_DIR C_COMPILER)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${CMAKE_CURRENT_LIST_DIR}" -B "${BINARY_DIR}"
    "-DCOG_SOURCE_DIR=${COG_SOURCE_DIR}" "-DCMAKE_C_COMPILER=${C_COMPILER}"
    RESULT_VARIABLE configure_result OUTPUT_VARIABLE configure_output ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "Loader-only configure failed:\n${configure_output}\n${configure_error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BINARY_DIR}" --parallel 2
    RESULT_VARIABLE build_result OUTPUT_VARIABLE build_output ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "Loader-only compile failed:\n${build_output}\n${build_error}")
endif()
