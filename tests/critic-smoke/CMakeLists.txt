# Compile installed public contracts individually, alongside the maintained
# type-use fixtures. Private implementation/vendor headers are not public TUs.
file(GLOB public_include_dirs LIST_DIRECTORIES true
    "${PROJECT_SOURCE_DIR}/libraries/*/include")
list(APPEND public_include_dirs
    "${PROJECT_SOURCE_DIR}/applications/cog-cli/include")

set(public_probes)
foreach(public_include_dir IN LISTS public_include_dirs)
    file(GLOB_RECURSE public_headers CONFIGURE_DEPENDS
        "${public_include_dir}/*.h")
    foreach(public_header IN LISTS public_headers)
        file(RELATIVE_PATH header_name "${public_include_dir}" "${public_header}")
        file(RELATIVE_PATH header_path "${PROJECT_SOURCE_DIR}" "${public_header}")
        string(MAKE_C_IDENTIFIER "${header_path}" probe_name)
        set(probe "${CMAKE_CURRENT_BINARY_DIR}/public_${probe_name}.c")
        configure_file(public_header.c.in "${probe}" @ONLY)
        list(APPEND public_probes "${probe}")
    endforeach()
endforeach()
file(GLOB smoke_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

add_library(critic-header-smoke OBJECT ${public_probes} ${smoke_sources})
target_include_directories(critic-header-smoke PRIVATE ${public_include_dirs})
target_enable_warnings(critic-header-smoke)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # This compile-only test stays strict and keeps assertions active even when
    # the surrounding production configuration does not enable Werror.
    target_compile_options(critic-header-smoke PRIVATE -Werror -UNDEBUG)
endif()

add_test(NAME critic-header-smoke COMMAND ${CMAKE_COMMAND}
    --build "${CMAKE_BINARY_DIR}" --target critic-header-smoke --config "$<CONFIG>")
# This test invokes the build tool; avoid competing build/install test commands.
set_tests_properties(critic-header-smoke PROPERTIES LABELS headers RUN_SERIAL TRUE TIMEOUT 60)
