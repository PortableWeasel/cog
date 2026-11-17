# Prefer the conventional checkout-local dependency once it has been built.
# Empty values from an older offline cache must not suppress this default.
if(NOT DEFINED COG_NET_CURL_BUILD_DIR)
    set(COG_NET_CURL_BUILD_DIR "" CACHE PATH "Audited c-ares-only curl build directory")
endif()
if(NOT DEFINED COG_NET_CURL_LIBRARY)
    set(COG_NET_CURL_LIBRARY "" CACHE FILEPATH "Shared curl library produced by audited build")
endif()
if(NOT COG_NET_CURL_BUILD_DIR AND
   EXISTS "${PROJECT_SOURCE_DIR}/build-deps/curl/CMakeCache.txt")
    set(COG_NET_CURL_BUILD_DIR "${PROJECT_SOURCE_DIR}/build-deps/curl")
endif()
if(COG_NET_CURL_BUILD_DIR)
    if(NOT COG_NET_CURL_LIBRARY)
        set(COG_NET_CURL_LIBRARY "${COG_NET_CURL_BUILD_DIR}/lib/libcurl.so")
    endif()
    if(NOT EXISTS "${COG_NET_CURL_BUILD_DIR}/CMakeCache.txt" OR
       NOT EXISTS "${COG_NET_CURL_LIBRARY}")
        message(FATAL_ERROR "Cog curl provenance requires a build cache and its shared library")
    endif()
    # The source cache supplies matching public headers. This is not a nested
    # include of that cache, which would import unrelated dependency settings.
    file(STRINGS "${COG_NET_CURL_BUILD_DIR}/CMakeCache.txt" _curl_source
        REGEX "^CURL_SOURCE_DIR:STATIC=")
    string(REGEX REPLACE "^CURL_SOURCE_DIR:STATIC=" "" _curl_source "${_curl_source}")
    if(NOT DEFINED COG_NET_CURL_INCLUDE_DIR)
        set(COG_NET_CURL_INCLUDE_DIR "" CACHE PATH
            "Public headers matching the audited curl library (empty selects source headers)")
    endif()
    if(NOT COG_NET_CURL_INCLUDE_DIR)
        set(COG_NET_CURL_INCLUDE_DIR "${_curl_source}/include")
    endif()
    if(NOT EXISTS "${COG_NET_CURL_INCLUDE_DIR}/curl/curl.h")
        message(FATAL_ERROR "Set COG_NET_CURL_INCLUDE_DIR to the audited curl headers")
    endif()
    # FindCURL otherwise reuses stale system RELEASE/DEBUG paths or a package
    # config target. All configurations must link the artifact Cog admits.
    set(CURL_NO_CURL_CMAKE TRUE)
    set(CURL_INCLUDE_DIR "${COG_NET_CURL_INCLUDE_DIR}")
    set(CURL_LIBRARY "${COG_NET_CURL_LIBRARY}")
    set(CURL_LIBRARY_RELEASE "${COG_NET_CURL_LIBRARY}")
    set(CURL_LIBRARY_DEBUG "${COG_NET_CURL_LIBRARY}")
    message(STATUS "Cog live transport: ${COG_NET_CURL_LIBRARY}")
elseif(NOT COG_NET_CURL_LIBRARY)
    message(STATUS "Cog live transport disabled: build build-deps/curl with the c-ares profile, or set COG_NET_CURL_BUILD_DIR (see libraries/cog-net/cmake/README.md)")
endif()
