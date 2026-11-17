set(COG_TEST_CARES_BUILD_DIR "" CACHE PATH "Relocatable c-ares build for the R18 installed consumer fixture")
if(BUILD_TESTING AND COG_BUILD_CLI AND COG_NET_CURL_BUILD_DIR AND COG_NET_CURL_LIBRARY
   AND COG_TEST_CARES_BUILD_DIR AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME unit-installed-path-audit
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_path_audit.py)
    add_test(NAME unit-installed-process-owner
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_process_owner.py ${PROJECT_BINARY_DIR})
    set_tests_properties(unit-installed-process-owner PROPERTIES TIMEOUT 30)
    add_test(NAME integration-installed-embedder
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run.py
            --build ${PROJECT_BINARY_DIR}
            --compiler ${CMAKE_C_COMPILER}
            --curl-build ${COG_NET_CURL_BUILD_DIR}
            --curl-library ${COG_NET_CURL_LIBRARY}
            --cares-build ${COG_TEST_CARES_BUILD_DIR}
            --asan $<BOOL:${COG_ENABLE_ASAN}>)
    set_tests_properties(integration-installed-embedder PROPERTIES TIMEOUT 120 LABELS "installed;embedding" RUN_SERIAL TRUE)
endif()

if(BUILD_TESTING AND NOT COG_BUILD_CLI)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME integration-installed-runtime-only
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_runtime_only.py
            ${PROJECT_BINARY_DIR} ${CMAKE_C_COMPILER})
    set_tests_properties(integration-installed-runtime-only PROPERTIES
        TIMEOUT 60 LABELS "installed;embedding;runtime" RUN_SERIAL TRUE)
endif()
