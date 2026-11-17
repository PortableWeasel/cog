# Network admission hashes the final curl artifact at configure time. Prepare
# the pinned dependency build first, reusing ExternalProject stamps thereafter.
if(NOT CMAKE_INSTALL_LIBDIR STREQUAL "lib")
    message(FATAL_ERROR "Vendored dependencies currently require CMAKE_INSTALL_LIBDIR=lib; use COG_USE_VENDORED_LIBS=OFF for another audited external layout")
endif()
set(COG_VENDOR_JOBS "2" CACHE STRING "Parallel jobs for vendored dependencies")
if(NOT COG_VENDOR_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "COG_VENDOR_JOBS must be a positive integer")
endif()
# Changes to recipes or pinned submodule revisions require reconfiguration so
# the linked artifact and recorded digest advance together on an ordinary build.
file(GLOB _vendor_recipes CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/vendor/*/CMakeLists.txt"
    "${PROJECT_SOURCE_DIR}/vendor/*/*.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_vendor_recipes}
    "${PROJECT_SOURCE_DIR}/vendor/CMakeLists.txt"
    "${PROJECT_SOURCE_DIR}/.gitmodules"
    "${PROJECT_SOURCE_DIR}/libraries/cog-net/cmake/curl-cares-profile.cmake"
    "${PROJECT_SOURCE_DIR}/libraries/cog-net/cmake/relocatable/curl-8.23.0-relocatable-metadata.patch"
    "${PROJECT_SOURCE_DIR}/libraries/cog-net/cmake/relocatable/c-ares-1.34.9-relocatable-metadata.patch")
find_package(Git REQUIRED)
foreach(_dependency curl c-ares openssl yyjson)
    execute_process(COMMAND "${GIT_EXECUTABLE}"
        -C "${PROJECT_SOURCE_DIR}/vendor/${_dependency}/upstream"
        rev-parse --git-path HEAD OUTPUT_VARIABLE _dependency_head
        OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _head_status)
    if(NOT _head_status EQUAL 0)
        message(FATAL_ERROR "Missing ${_dependency} submodule; run git submodule update --init --recursive")
    endif()
    get_filename_component(_dependency_head "${_dependency_head}" ABSOLUTE
        BASE_DIR "${PROJECT_SOURCE_DIR}/vendor/${_dependency}/upstream")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_dependency_head}")
endforeach()
set(_vendor_binary "${PROJECT_BINARY_DIR}/vendor")
file(MAKE_DIRECTORY "${_vendor_binary}")
message(STATUS "Preparing vendored networking libraries (logs: ${_vendor_binary})")
execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${PROJECT_SOURCE_DIR}/vendor" -B "${_vendor_binary}"
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DCOG_VENDOR_JOBS=${COG_VENDOR_JOBS}"
    RESULT_VARIABLE _vendor_status
    OUTPUT_FILE "${_vendor_binary}/configure.log"
    ERROR_FILE "${_vendor_binary}/configure-error.log")
if(NOT _vendor_status EQUAL 0)
    message(FATAL_ERROR "Vendored dependency configure failed (initialize sources with git submodule update --init --recursive); see ${_vendor_binary}/configure-error.log and configure.log")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_vendor_binary}"
    --parallel "${COG_VENDOR_JOBS}"
    RESULT_VARIABLE _vendor_status
    OUTPUT_FILE "${_vendor_binary}/build.log"
    ERROR_FILE "${_vendor_binary}/build-error.log")
if(NOT _vendor_status EQUAL 0)
    message(FATAL_ERROR "Vendored dependency build failed; see ${_vendor_binary}/build-error.log and build.log")
endif()
set(_vendor_prefix "${_vendor_binary}/curl-build/install")
set(COG_NET_CURL_BUILD_DIR "${_vendor_binary}/curl-build")
set(COG_NET_CURL_LIBRARY "${_vendor_prefix}/lib/libcurl.so")
set(COG_NET_CURL_INCLUDE_DIR "${_vendor_prefix}/include")
set(COG_TEST_CARES_BUILD_DIR "" CACHE PATH "External c-ares build for installed consumer checks")
set(COG_TEST_CARES_BUILD_DIR "${_vendor_binary}/cares-build")
set(OPENSSL_ROOT_DIR "${_vendor_prefix}")
set(OPENSSL_INCLUDE_DIR "${_vendor_prefix}/include")
set(OPENSSL_CRYPTO_LIBRARY "${_vendor_prefix}/lib/libcrypto.so")
set(OPENSSL_SSL_LIBRARY "${_vendor_prefix}/lib/libssl.so")
# Ship the same dependency bytes that net admission records, plus their headers,
# metadata and license notices. Library RUNPATHs are owned by vendor wrappers.
install(DIRECTORY "${_vendor_prefix}/" DESTINATION "." USE_SOURCE_PERMISSIONS)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${COG_NET_CURL_LIBRARY}" "${_vendor_prefix}/lib/libcares.so"
    "${OPENSSL_CRYPTO_LIBRARY}" "${OPENSSL_SSL_LIBRARY}")
