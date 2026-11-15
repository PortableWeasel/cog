# -----------------------------------------------------------------------
# cog-toolkit — tool execution framework
# -----------------------------------------------------------------------

# Stage 12.4b: cog-toolkit is now a real static library.  It compiles the
# call/exec/result modules into an archive while preserving the public
# include path, the C11 compile feature, the cog::toolkit alias, and the
# strict-warning settings that downstream INTERFACE consumers rely on.
add_library(cog-toolkit STATIC
    src/toolkit_call.c
    src/toolkit_exec.c
    src/toolkit_result.c
)
add_library(cog::toolkit ALIAS cog-toolkit)

target_include_directories(cog-toolkit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-toolkit PUBLIC c_std_11)
target_link_libraries(cog-toolkit PUBLIC cog::core)

# Apply the project-wide strict-warning policy (and ASan/UBSan when enabled)
# to the production sources.
target_enable_warnings(cog-toolkit)

# -----------------------------------------------------------------------
# Unit test — contract test for the toolkit (see contracts/CONTRACT.md)
#
# The test is registered here (not in tests/unit/CMakeLists.txt) because
# it links the cog-toolkit archive; the shared wiring in
# tests/unit/CMakeLists.txt is owned by integration ticket 1433731.
# -----------------------------------------------------------------------
add_executable(test_toolkit
    ${PROJECT_SOURCE_DIR}/tests/unit/test_toolkit.c
)
target_link_libraries(test_toolkit PRIVATE cog::toolkit)
target_enable_warnings(test_toolkit)

add_test(NAME unit-toolkit
    COMMAND test_toolkit
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# -----------------------------------------------------------------------
# Install rules
# -----------------------------------------------------------------------
include(GNUInstallDirs)

install(DIRECTORY include/cog
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS cog-toolkit
    EXPORT cog-toolkit-targets)

install(EXPORT cog-toolkit-targets
    FILE cog-toolkit-targets.cmake
    NAMESPACE cog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog)