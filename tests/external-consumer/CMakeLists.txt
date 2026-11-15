set(COG_TEST_CARES_BUILD_DIR "" CACHE PATH "Relocatable c-ares build for the R18 installed consumer fixture")
if(BUILD_TESTING AND COG_BUILD_CLI AND COG_NET_CURL_BUILD_DIR AND COG_NET_CURL_LIBRARY
   AND COG_TEST_CARES_BUILD_DIR AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
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
