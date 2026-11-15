# -----------------------------------------------------------------------
# cog-cli — command-line application and scenario mock harness
#
# The CLI is split into a testable static library (cog-cli-lib, alias
# cog::cli) containing cli.c and mock_provider.c, and a thin executable
# (cog-cli) that only parses options, installs signal handlers, and runs
# the app.  Scenario tests (Stages 12.6b–12.6e) link cog::cli and drive
# the app programmatically instead of spawning the binary.
# -----------------------------------------------------------------------

add_library(cog-cli-lib STATIC
    src/cli.c
    src/mock_provider.c
)
add_library(cog::cli ALIAS cog-cli-lib)

target_include_directories(cog-cli-lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-cli-lib PUBLIC c_std_11)

# cli.c and mock_provider.c use POSIX facilities (select, nanosleep,
# signal); the _POSIX_C_SOURCE define lives in each source file.
target_link_libraries(cog-cli-lib PUBLIC cog::core cog::provider cog::obs)

target_enable_warnings(cog-cli-lib)

add_executable(cog-cli src/main.c)
target_link_libraries(cog-cli PRIVATE cog::cli)
target_enable_warnings(cog-cli)

# -----------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()