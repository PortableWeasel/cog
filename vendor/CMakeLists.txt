cmake_minimum_required(VERSION 3.16)
project(cog-vendor LANGUAGES C)
include(ExternalProject)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Perl REQUIRED)
find_program(COG_VENDOR_MAKE NAMES gmake make)
find_program(COG_VENDOR_GIT NAMES git)
find_program(COG_VENDOR_PATCH NAMES patch)
if(NOT COG_VENDOR_MAKE OR NOT COG_VENDOR_PATCH)
    message(FATAL_ERROR "Vendored builds require make and patch (and git for submodule checkouts)")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Cog's admitted shared dependency profile currently requires Linux")
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Dependency build type" FORCE)
endif()
set(COG_VENDOR_JOBS 2 CACHE STRING "Parallel jobs per dependency build")
if(NOT COG_VENDOR_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "COG_VENDOR_JOBS must be a positive integer")
endif()
set(COG_VENDOR_PREFIX "${CMAKE_BINARY_DIR}/curl-build/install")
set(_vendor_common
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DCMAKE_INSTALL_PREFIX=${COG_VENDOR_PREFIX}"
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON
    "-DCMAKE_INSTALL_RPATH=$ORIGIN"
    -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF
    -DCMAKE_BUILD_RPATH_USE_ORIGIN=ON)

include("${CMAKE_CURRENT_SOURCE_DIR}/source-revisions.cmake")

function(cog_vendor_require_source name marker)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/upstream/${marker}")
        message(FATAL_ERROR "Missing ${name} submodule: run git submodule update --init --recursive")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/upstream/.git")
        if(NOT COG_VENDOR_GIT)
            message(FATAL_ERROR "Submodule checkouts require git")
        endif()
        execute_process(COMMAND "${COG_VENDOR_GIT}" -C "${CMAKE_CURRENT_SOURCE_DIR}/upstream"
            rev-parse HEAD OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE revision_result)
        if(NOT revision_result EQUAL 0)
            message(FATAL_ERROR "Cannot identify ${name} source revision")
        endif()
        get_filename_component(dependency "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
        if(NOT revision STREQUAL COG_SOURCE_REVISION_${dependency})
            message(FATAL_ERROR "Update vendor/source-revisions.cmake for the ${name} submodule pin")
        endif()
        execute_process(COMMAND "${COG_VENDOR_GIT}" -C "${CMAKE_CURRENT_SOURCE_DIR}/upstream"
            status --porcelain OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE status_result)
        if(NOT status_result EQUAL 0 OR dirty)
            message(FATAL_ERROR "${name} upstream must be a clean submodule; keep Cog changes in vendor/${name} wrappers")
        endif()
    else()
        # A CPack source archive contains the complete pinned source trees.
        # Do not let git accidentally discover an unrelated parent repository.
        get_filename_component(dependency "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
        set(revision "${COG_SOURCE_REVISION_${dependency}}")
        if(NOT revision)
            message(FATAL_ERROR "No source archive revision for ${name}")
        endif()
        message(STATUS "${name}: using populated source archive revision ${revision}")
    endif()
    set(COG_VENDOR_REVISION "${revision}" PARENT_SCOPE)
endfunction()

add_subdirectory(openssl)
add_subdirectory(c-ares)
add_subdirectory(curl)
