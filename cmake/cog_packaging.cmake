# Set metadata before include(CPack), which snapshots these variables.
set(CPACK_PACKAGE_NAME "cog")
set(CPACK_PACKAGE_VENDOR "Cog Authors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_FILE_NAME "cog-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_VERBATIM_VARIABLES ON)
if(NOT DEFINED CPACK_GENERATOR)
    set(CPACK_GENERATOR TGZ)
endif()
if(NOT DEFINED CPACK_SOURCE_GENERATOR)
    set(CPACK_SOURCE_GENERATOR TGZ)
endif()
set(CPACK_SOURCE_PACKAGE_FILE_NAME "cog-${PROJECT_VERSION}-source")
# Root-local builds/staging and local agent state must never enter an archive.
# Anchor build exclusions at the source root: upstream build metadata is needed.
set(CPACK_SOURCE_IGNORE_FILES
    "${PROJECT_SOURCE_DIR}/build[^/]*(/|$)"
    "${PROJECT_SOURCE_DIR}/install[^/]*(/|$)"
    "${PROJECT_SOURCE_DIR}/__cmake_systeminformation(/|$)"
    "/[.]git(/|$)" "/[.]pi(/|$)" "/[.]codex(/|$)"
    "/[.]env([^/]*)(/|$)" "/__pycache__(/|$)" "[.]pyc$"
    "/[.]vscode(/|$)" "/[.]idea(/|$)" "[.]sw[op]$" "~$"
    "/CMakeUserPresets[.]json$")
# Refresh checkout exclusions when cpack runs, including files created after
# configure. A generated hook keeps source paths out of the reusable template.
configure_file("${PROJECT_SOURCE_DIR}/cmake/cog_cpack_options.cmake.in"
    "${PROJECT_BINARY_DIR}/cog_cpack_options.cmake" @ONLY)
set(CPACK_PROJECT_CONFIG_FILE "${PROJECT_BINARY_DIR}/cog_cpack_options.cmake")
# A project license has not been supplied. Do not point CPack at a nonexistent
# file or silently substitute a dependency license for Cog's own terms.
if(EXISTS "${PROJECT_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/cog")
endif()
install(FILES "${PROJECT_SOURCE_DIR}/docs/PACKAGING.md"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}")
include(CPack)
