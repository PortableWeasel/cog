cog_vendor_require_source(curl CMakeLists.txt)
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _recipe_hash)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/../curl/prepare_source.py" _prepare_hash)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/../../libraries/cog-net/cmake/relocatable/curl-8.23.0-relocatable-metadata.patch" _patch_hash)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/../../libraries/cog-net/cmake/curl-cares-profile.cmake" _profile_hash)
ExternalProject_Add(cog-vendor-curl
    DEPENDS cog-vendor-c-ares cog-vendor-openssl
    SOURCE_DIR "${CMAKE_BINARY_DIR}/sources/curl"
    BINARY_DIR "${CMAKE_BINARY_DIR}/curl-build"
    DOWNLOAD_COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_source.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream" <SOURCE_DIR>
        "${CMAKE_CURRENT_SOURCE_DIR}/../../libraries/cog-net/cmake/relocatable/curl-8.23.0-relocatable-metadata.patch"
        "${COG_VENDOR_REVISION}" "${_recipe_hash}-${_prepare_hash}-${_patch_hash}-${_profile_hash}"
    UPDATE_COMMAND ""
    CMAKE_ARGS -C "${CMAKE_CURRENT_SOURCE_DIR}/../../libraries/cog-net/cmake/curl-cares-profile.cmake"
        ${_vendor_common}
        "-DCMAKE_PREFIX_PATH=${COG_VENDOR_PREFIX}"
        "-DOPENSSL_ROOT_DIR=${COG_VENDOR_PREFIX}"
        "-DOPENSSL_INCLUDE_DIR=${COG_VENDOR_PREFIX}/include"
        "-DOPENSSL_SSL_LIBRARY=${COG_VENDOR_PREFIX}/lib/libssl.so"
        "-DOPENSSL_CRYPTO_LIBRARY=${COG_VENDOR_PREFIX}/lib/libcrypto.so"
        "-DCARES_INCLUDE_DIR=${COG_VENDOR_PREFIX}/include"
        "-DCARES_LIBRARY=${COG_VENDOR_PREFIX}/lib/libcares.so"
        -DCURL_USE_CMAKECONFIG=ON -DCURL_USE_PKGCONFIG=OFF
        -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF
        -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON
        -DBUILD_EXAMPLES=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --parallel "${COG_VENDOR_JOBS}"
    INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR>
    BUILD_BYPRODUCTS "${COG_VENDOR_PREFIX}/lib/libcurl.so")
