cog_vendor_require_source(c-ares CMakeLists.txt)
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _recipe_hash)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/../curl/prepare_source.py" _prepare_hash)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/../../libraries/cog-net/cmake/relocatable/c-ares-1.34.9-relocatable-metadata.patch" _patch_hash)
ExternalProject_Add(cog-vendor-c-ares
    DEPENDS cog-vendor-openssl
    SOURCE_DIR "${CMAKE_BINARY_DIR}/sources/c-ares"
    BINARY_DIR "${CMAKE_BINARY_DIR}/cares-build"
    DOWNLOAD_COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../curl/prepare_source.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream" <SOURCE_DIR>
        "${CMAKE_CURRENT_SOURCE_DIR}/../../libraries/cog-net/cmake/relocatable/c-ares-1.34.9-relocatable-metadata.patch"
        "${COG_VENDOR_REVISION}" "${_recipe_hash}-${_prepare_hash}-${_patch_hash}"
    UPDATE_COMMAND ""
    CMAKE_ARGS ${_vendor_common}
        -DCARES_SHARED=ON -DCARES_STATIC=OFF -DCARES_THREADS=ON
        -DCARES_BUILD_TOOLS=OFF -DCARES_BUILD_TESTS=OFF
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --parallel "${COG_VENDOR_JOBS}"
    INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR>
    BUILD_BYPRODUCTS "${COG_VENDOR_PREFIX}/lib/libcares.so")
