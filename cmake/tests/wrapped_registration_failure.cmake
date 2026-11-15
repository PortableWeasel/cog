cmake_minimum_required(VERSION 3.16)
foreach(style plain typed)
    set(build "${PROBE_ROOT}/${style}")
    execute_process(COMMAND "${CMAKE_COMMAND}"
        -S "${COG_SOURCE_ROOT}/cmake/tests/wrapped_probe" -B "${build}"
        -G "${PROBE_GENERATOR}" "-DCMAKE_C_COMPILER=${PROBE_C_COMPILER}"
        "-DCOG_SOURCE_ROOT=${COG_SOURCE_ROOT}" "-DWRAPPER_STYLE=${style}"
        "-DCMAKE_BUILD_TYPE=${PROBE_CONFIG}"
        RESULT_VARIABLE configured OUTPUT_VARIABLE output ERROR_VARIABLE error
        TIMEOUT 30)
    if(NOT "${configured}" STREQUAL "0")
        message(FATAL_ERROR "Probe configure failed: ${output}\n${error}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DBUILD_ROOT=${build}"
        "-DASSERTION_MANIFEST=${build}/test-assertions-${PROBE_CONFIG}.cmake"
        "-DCTEST_CONFIGURATION_TYPE=${PROBE_CONFIG}"
        -P "${COG_SOURCE_ROOT}/cmake/tests/assertion_registration.cmake"
        RESULT_VARIABLE outcome OUTPUT_VARIABLE output ERROR_VARIABLE error
        TIMEOUT 5)
    if("${outcome}" STREQUAL "0" OR
       NOT error MATCHES "uses unclassified executable" OR
       NOT error MATCHES "custom_wrapped_probe")
        message(FATAL_ERROR "Wrapped ${style} target escaped classification: ${outcome}\n${output}\n${error}")
    endif()
endforeach()
