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
find_package(Threads REQUIRED)
target_link_libraries(cog-toolkit PUBLIC cog::core Threads::Threads)

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

# Critic regression tests for the acceptance-critical review questions
# (concurrency, timeout boundaries, cancellation, stop_on_error).
add_executable(test_toolkit_critic
    ${PROJECT_SOURCE_DIR}/tests/unit/test_toolkit_critic.c
)
target_link_libraries(test_toolkit_critic PRIVATE cog::toolkit)
target_enable_warnings(test_toolkit_critic)

add_test(NAME unit-toolkit-critic
    COMMAND test_toolkit_critic
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Bounded-worker scheduling plus deterministic pthread_create failure tests.
# The link wrap is private to this test executable and does not affect the
# production archive or other tests.
add_executable(test_toolkit_parallel
    ${PROJECT_SOURCE_DIR}/tests/unit/test_toolkit_parallel.c)
target_link_libraries(test_toolkit_parallel PRIVATE cog::toolkit)
target_enable_warnings(test_toolkit_parallel)
target_link_options(test_toolkit_parallel PRIVATE "-Wl,--wrap=pthread_create")

add_test(NAME unit-toolkit-parallel
    COMMAND test_toolkit_parallel
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