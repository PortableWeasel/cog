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
option(COG_ENABLE_WERROR "Treat compiler warnings as errors" OFF)

include(CTest)

# -----------------------------------------------------------------------
# Tooling helpers
# -----------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(compiler_warnings)

# -----------------------------------------------------------------------
# Libraries
# -----------------------------------------------------------------------
add_subdirectory(libraries/cog-core)
# add_subdirectory(libraries/cog-provider)
# add_subdirectory(libraries/cog-toolkit)

# -----------------------------------------------------------------------
# Applications
# -----------------------------------------------------------------------
if(COG_BUILD_CLI)
    # add_subdirectory(applications/cog-cli)
endif()

# -----------------------------------------------------------------------
# Proof-of-concepts
# -----------------------------------------------------------------------
add_subdirectory(pocs)

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