cog_vendor_require_source(OpenSSL Configure)
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _recipe_hash)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/relocate_metadata.py" _metadata_hash)
ExternalProject_Add(cog-vendor-openssl
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/upstream"
    BINARY_DIR "${CMAKE_BINARY_DIR}/openssl-build"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env "CC=${CMAKE_C_COMPILER}"
        "COG_VENDOR_REVISION=${COG_VENDOR_REVISION}"
        "COG_VENDOR_RECIPE=${_recipe_hash}-${_metadata_hash}"
        "${PERL_EXECUTABLE}" <SOURCE_DIR>/Configure shared no-tests no-engine no-module no-dso no-legacy
        "--prefix=${COG_VENDOR_PREFIX}" --libdir=lib --openssldir=/etc/ssl
        "-Wl,-rpath,'$$ORIGIN:$$ORIGIN/../lib'"
    BUILD_COMMAND "${COG_VENDOR_MAKE}" "-j${COG_VENDOR_JOBS}"
    INSTALL_COMMAND "${COG_VENDOR_MAKE}" install_sw
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/relocate_metadata.py"
            "${COG_VENDOR_PREFIX}" "${CMAKE_CURRENT_SOURCE_DIR}/upstream/LICENSE.txt"
    BUILD_BYPRODUCTS "${COG_VENDOR_PREFIX}/lib/libssl.so"
        "${COG_VENDOR_PREFIX}/lib/libcrypto.so")
