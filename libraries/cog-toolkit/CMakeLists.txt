# -----------------------------------------------------------------------
# cog-toolkit — tool execution framework
# -----------------------------------------------------------------------

# Pinned private codec object is absorbed into the installed archive. All
# exports are namespaced; consumers have no yyjson link/include dependency.
# Upstream sources remain unmodified and separate from Cog warning policy.
add_library(cog-toolkit-yyjson OBJECT vendor/yyjson/yyjson.c)
target_compile_features(cog-toolkit-yyjson PRIVATE c_std_11)
target_compile_options(cog-toolkit-yyjson PRIVATE
    -include ${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson/prefix.h)

if(COG_ENABLE_ASAN)
    target_compile_options(cog-toolkit-yyjson PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()
if(COG_ENABLE_TSAN)
    target_compile_options(cog-toolkit-yyjson PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
endif()

add_library(cog-toolkit STATIC
    src/toolkit_call.c
    src/toolkit_exec.c
    src/toolkit_result.c
    src/toolkit_json.c
    src/toolkit_execution.c
    src/workspace.c
    src/workspace_process.c
    $<TARGET_OBJECTS:cog-toolkit-yyjson>
)
add_library(cog::toolkit ALIAS cog-toolkit)

# Export as cog::toolkit (matching the build-tree alias) so installed
# consumers use the same target name as in-tree consumers.
set_target_properties(cog-toolkit PROPERTIES EXPORT_NAME toolkit)

target_include_directories(cog-toolkit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-toolkit PUBLIC c_std_11)
target_include_directories(cog-toolkit SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson)
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
# tests/unit/CMakeLists.txt is shared with other delivery packages.
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
install(FILES vendor/yyjson/YYJSON-LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/cog-toolkit RENAME yyjson-LICENSE)

add_executable(test_toolkit_async ${PROJECT_SOURCE_DIR}/tests/unit/test_toolkit_async.c)
target_link_libraries(test_toolkit_async PRIVATE cog::toolkit)
target_enable_warnings(test_toolkit_async)
add_test(NAME unit-toolkit-async COMMAND test_toolkit_async)
add_executable(test_workspace ${PROJECT_SOURCE_DIR}/tests/unit/test_workspace.c)
target_link_libraries(test_workspace PRIVATE cog::toolkit)
target_enable_warnings(test_workspace)
add_test(NAME unit-workspace COMMAND test_workspace)
set_tests_properties(unit-workspace PROPERTIES TIMEOUT 15)
add_executable(test_toolkit_resources ${PROJECT_SOURCE_DIR}/tests/unit/test_toolkit_resources.c)
target_link_libraries(test_toolkit_resources PRIVATE cog::toolkit)
target_enable_warnings(test_toolkit_resources)
target_link_options(test_toolkit_resources PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
add_test(NAME unit-toolkit-resources COMMAND test_toolkit_resources)
add_executable(test_toolkit_fencing ${PROJECT_SOURCE_DIR}/tests/unit/test_toolkit_fencing.c)
target_link_libraries(test_toolkit_fencing PRIVATE cog::toolkit)
target_enable_warnings(test_toolkit_fencing)
add_test(NAME unit-toolkit-fencing COMMAND test_toolkit_fencing)
