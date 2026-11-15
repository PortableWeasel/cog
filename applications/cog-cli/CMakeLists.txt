# -----------------------------------------------------------------------
# cog-cli — command-line application and scenario mock harness
#
# The CLI is split into a testable static library (cog-cli-lib, alias
# cog::cli) containing cli.c and mock_provider.c, and a thin executable
# (cog-cli) that only parses options, installs signal handlers, and runs
# the app.  Scenario tests (Stages 12.6b–12.6e) link cog::cli and drive
# the app programmatically instead of spawning the binary.
# -----------------------------------------------------------------------

include(GNUInstallDirs)

# Configuration is independently buildable while H/K migrate the old app.
add_library(cog-cli-config STATIC src/options.c src/configuration.c)
add_library(cog::cli-config ALIAS cog-cli-config)
set_target_properties(cog-cli-config PROPERTIES EXPORT_NAME cli-config)
target_compile_features(cog-cli-config PUBLIC c_std_11)
target_include_directories(cog-cli-config PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
foreach(module core provider toolkit net obs ipc)
    target_include_directories(cog-cli-config PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/libraries/cog-${module}/include>)
endforeach()
target_enable_warnings(cog-cli-config)

add_library(cog-cli-lib STATIC
    src/cli.c
    src/mock_provider.c
)
add_library(cog::cli ALIAS cog-cli-lib)
set_target_properties(cog-cli-lib PROPERTIES EXPORT_NAME cli)

target_include_directories(cog-cli-lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-cli-lib PUBLIC c_std_11)

# cli.c and mock_provider.c use POSIX facilities (select, nanosleep,
# signal); the _POSIX_C_SOURCE define lives in each source file.
target_link_libraries(cog-cli-lib PUBLIC cog::core cog::provider cog::obs cog::toolkit cog::net cog-cli-config)

target_enable_warnings(cog-cli-lib)

add_executable(cog-cli src/main.c)
target_link_libraries(cog-cli PRIVATE cog::cli)
target_enable_warnings(cog-cli)

# -----------------------------------------------------------------------
# Install rules
# -----------------------------------------------------------------------
install(TARGETS cog-cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(TARGETS cog-cli-lib cog-cli-config
    EXPORT cog-cli-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(DIRECTORY include/cog_cli
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT cog-cli-targets
    FILE cog-cli-targets.cmake
    NAMESPACE cog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog)

# -----------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()