# -----------------------------------------------------------------------
# cog-orchestrator — process lifecycle, health tracking, placement
# -----------------------------------------------------------------------

# Stage 12.5: cog-orchestrator is now a real static library.  It compiles
# src/orchestrator.c into an archive while preserving the public include
# path, the C11 compile feature, the cog::orchestrator alias, and the
# strict-warning settings that downstream consumers rely on.
#
# The orchestrator uses the IPC fabric (cog::ipc) for its listener socket
# and the cog-core result codes / public headers (cog::core).  cog::core
# already propagates cog::ipc and cog::obs, so linking both is safe and
# keeps the dependency graph acyclic (cog-orchestrator is a leaf).
add_library(cog-orchestrator STATIC
    src/orchestrator.c
    src/orchestrator_wire.c
)
add_library(cog::orchestrator ALIAS cog-orchestrator)

# Export as cog::orchestrator (matching the build-tree alias) so installed
# consumers use the same target name as in-tree consumers.
set_target_properties(cog-orchestrator PROPERTIES EXPORT_NAME orchestrator)

target_include_directories(cog-orchestrator
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-orchestrator PUBLIC c_std_11)
target_link_libraries(cog-orchestrator PUBLIC cog::core cog::ipc)

# Apply the project-wide strict-warning policy (and ASan/UBSan when enabled)
# to the production source.
target_enable_warnings(cog-orchestrator)

# -----------------------------------------------------------------------
# Install rules
# -----------------------------------------------------------------------
include(GNUInstallDirs)

install(DIRECTORY include/cog
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS cog-orchestrator
    EXPORT cog-orchestrator-targets)

install(EXPORT cog-orchestrator-targets
    FILE cog-orchestrator-targets.cmake
    NAMESPACE cog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog)