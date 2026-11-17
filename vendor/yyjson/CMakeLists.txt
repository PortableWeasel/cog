# Compile the pinned upstream source separately for each private namespace.
# The object is absorbed into its owner archive; no yyjson target/header leaks
# into Cog's public installed dependency graph.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/upstream/src/yyjson.c")
    message(FATAL_ERROR "Missing yyjson submodule; run git submodule update --init --recursive")
endif()

function(cog_vendor_yyjson target component)
    if(NOT component MATCHES "^(provider|toolkit)$")
        message(FATAL_ERROR "Unknown yyjson namespace: ${component}")
    endif()
    set(_yyjson_root "${PROJECT_SOURCE_DIR}/vendor/yyjson")
    add_library(${target} OBJECT "${_yyjson_root}/upstream/src/yyjson.c")
    target_compile_features(${target} PRIVATE c_std_11)
    target_compile_options(${target} PRIVATE
        -include "${_yyjson_root}/${component}_prefix.h")
    if(COG_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
    if(COG_ENABLE_TSAN)
        target_compile_options(${target} PRIVATE
            -fsanitize=thread -fno-omit-frame-pointer)
    endif()
endfunction()

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME vendor-yyjson-prefixes
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_prefix.py" --check)
    set_tests_properties(vendor-yyjson-prefixes PROPERTIES LABELS "build-policy;vendor")
endif()
