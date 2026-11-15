cmake_minimum_required(VERSION 3.16)

project(cog
    VERSION 0.1.0
    DESCRIPTION "Cog — an LLM harness and agent framework for structured coding workflows"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

# -----------------------------------------------------------------------
# Options
# -----------------------------------------------------------------------
option(COG_BUILD_CLI "Build the CLI application" ON)
option(COG_ENABLE_ASAN "Enable AddressSanitizer + UBSan" OFF)
option(COG_ENABLE_TSAN "Enable ThreadSanitizer (mutually exclusive with ASan)" OFF)
option(COG_ENABLE_WERROR "Treat compiler warnings as errors" OFF)
option(COG_BUILD_BENCHMARKS "Build benchmark executables (opt-in; not part of the default CTest run)" OFF)

if(COG_ENABLE_ASAN AND COG_ENABLE_TSAN)
    message(FATAL_ERROR
        "COG_ENABLE_ASAN and COG_ENABLE_TSAN require separate builds")
endif()

include(CTest)

# -----------------------------------------------------------------------
# Tooling helpers
# -----------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(compiler_warnings)
if(BUILD_TESTING)
    include(test_assertions)
    add_subdirectory(cmake/tests/optin)
endif()

# -----------------------------------------------------------------------
# Libraries
# -----------------------------------------------------------------------
add_subdirectory(libraries/cog-core)
add_subdirectory(libraries/cog-provider)
add_subdirectory(libraries/cog-toolkit)
add_subdirectory(libraries/cog-ipc)
add_subdirectory(libraries/cog-obs)
add_subdirectory(libraries/cog-config)
add_subdirectory(libraries/cog-net)
add_subdirectory(libraries/cog-orchestrator)

# -----------------------------------------------------------------------
# Applications
# -----------------------------------------------------------------------
# The shared scenario tests link cog::cli, so define the application
# targets before descending into tests/scenarios.
if(COG_BUILD_CLI)
    add_subdirectory(applications/cog-cli)
endif()

# -----------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------
add_subdirectory(tests)

# -----------------------------------------------------------------------
# Proof-of-concepts
# -----------------------------------------------------------------------
add_subdirectory(pocs)

# Test-only assertions must remain active in every configuration. Finalize
# after all targets exist; production compile flags are unchanged.
if(BUILD_TESTING)
    add_subdirectory(cmake/tests)
    cog_configure_test_assertions()
endif()

# -----------------------------------------------------------------------
# Packaging
# -----------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CPack)

set(CPACK_PACKAGE_NAME          "${PROJECT_NAME}")
set(CPACK_PACKAGE_VENDOR        "Cog Authors")
set(CPACK_PACKAGE_VERSION       "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_CONTACT       "cog@example.com")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR             "TGZ;STGZ")
set(CPACK_VERBATIM_VARIABLES    ON)