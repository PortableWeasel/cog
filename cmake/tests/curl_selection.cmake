# Configure a small real FindCURL consumer: no synthetic imported target can
# hide stale per-configuration library paths from an existing CMake cache.
if(NOT DEFINED COG_SOURCE_ROOT OR NOT DEFINED PROBE_ROOT)
    message(FATAL_ERROR "COG_SOURCE_ROOT and PROBE_ROOT are required")
endif()
file(REMOVE_RECURSE "${PROBE_ROOT}")
file(MAKE_DIRECTORY "${PROBE_ROOT}/source")
file(WRITE "${PROBE_ROOT}/source/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.16)
project(curl_selection NONE)
include("${COG_SOURCE_ROOT}/cmake/cog_curl.cmake")
find_package(CURL MODULE REQUIRED)
if(EXPECT_LIBRARY)
    foreach(property IMPORTED_LOCATION IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION_DEBUG)
        get_target_property(actual CURL::libcurl ${property})
        if(NOT actual STREQUAL EXPECT_LIBRARY)
            message(FATAL_ERROR "${property}: expected ${EXPECT_LIBRARY}, got ${actual}")
        endif()
    endforeach()
    get_target_property(actual CURL::libcurl INTERFACE_INCLUDE_DIRECTORIES)
    if(NOT actual STREQUAL EXPECT_INCLUDE)
        message(FATAL_ERROR "Expected matching headers ${EXPECT_INCLUDE}, got ${actual}")
    endif()
elseif(COG_NET_CURL_BUILD_DIR OR COG_NET_CURL_LIBRARY)
    message(FATAL_ERROR "Offline configuration unexpectedly selected a profile")
endif()
]=])

function(fake_dependency root)
    file(MAKE_DIRECTORY "${root}/lib" "${root}/source/include/curl")
    file(WRITE "${root}/CMakeCache.txt"
        "CURL_SOURCE_DIR:STATIC=${root}/source\nENABLE_ARES:BOOL=ON\nENABLE_THREADED_RESOLVER:BOOL=OFF\n")
    file(WRITE "${root}/lib/libcurl.so" "configure-only fixture")
    file(WRITE "${root}/source/include/curl/curl.h" "/* configure-only fixture */\n")
    file(WRITE "${root}/source/include/curl/curlver.h" "#define LIBCURL_VERSION \"8.0.0\"\n")
endfunction()

function(configure_case name)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        -S "${PROBE_ROOT}/source" -B "${PROBE_ROOT}/${name}"
        "-DCOG_SOURCE_ROOT=${COG_SOURCE_ROOT}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name} configuration failed:\n${output}\n${error}")
    endif()
endfunction()

# Start with an actual offline cache, then add the conventional dependency.
# This reproduces the user's workspace migration, including FindCURL caches.
configure_case(migration -DCMAKE_BUILD_TYPE=Debug
    -DCOG_NET_CURL_BUILD_DIR:PATH= -DCOG_NET_CURL_LIBRARY:FILEPATH=)
set(local "${PROBE_ROOT}/source/build-deps/curl")
fake_dependency("${local}")
configure_case(migration -DCMAKE_BUILD_TYPE=Debug
    "-DEXPECT_LIBRARY=${local}/lib/libcurl.so"
    "-DEXPECT_INCLUDE=${local}/source/include")
configure_case(migration -DCMAKE_BUILD_TYPE=Release
    "-DEXPECT_LIBRARY=${local}/lib/libcurl.so"
    "-DEXPECT_INCLUDE=${local}/source/include")
configure_case(fresh "-DEXPECT_LIBRARY=${local}/lib/libcurl.so"
    "-DEXPECT_INCLUDE=${local}/source/include")

# A caller may select installed bytes under a different audited build.
set(explicit "${PROBE_ROOT}/explicit")
fake_dependency("${explicit}")
file(MAKE_DIRECTORY "${explicit}/install/lib")
file(WRITE "${explicit}/install/lib/libcurl.so" "alternate installed fixture")
configure_case(explicit "-DCOG_NET_CURL_BUILD_DIR=${explicit}"
    "-DCOG_NET_CURL_LIBRARY=${explicit}/install/lib/libcurl.so"
    "-DEXPECT_LIBRARY=${explicit}/install/lib/libcurl.so"
    "-DEXPECT_INCLUDE=${explicit}/source/include")


# Change only the build directory in an already-configured automatic build.
# Derived library/header defaults must follow it instead of becoming cached
# inputs that silently refer to the previous dependency.
configure_case(migration "-DCOG_NET_CURL_BUILD_DIR=${explicit}"
    "-DEXPECT_LIBRARY=${explicit}/lib/libcurl.so"
    "-DEXPECT_INCLUDE=${explicit}/source/include")
configure_case(migration -DCOG_NET_CURL_BUILD_DIR:PATH=
    "-DEXPECT_LIBRARY=${local}/lib/libcurl.so"
    "-DEXPECT_INCLUDE=${local}/source/include")

# A selected profile with missing bytes must stop during configuration.
execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${PROBE_ROOT}/source" -B "${PROBE_ROOT}/missing"
    "-DCOG_SOURCE_ROOT=${COG_SOURCE_ROOT}"
    "-DCOG_NET_CURL_BUILD_DIR=${explicit}"
    "-DCOG_NET_CURL_LIBRARY=${explicit}/missing.so"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR NOT error MATCHES "Cog curl provenance requires")
    message(FATAL_ERROR "Missing selected library did not fail clearly:\n${output}\n${error}")
endif()
