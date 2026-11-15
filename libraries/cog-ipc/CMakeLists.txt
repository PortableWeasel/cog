# -----------------------------------------------------------------------
# cog-ipc — IPC fabric transport layer over Unix domain sockets.
# -----------------------------------------------------------------------

# Stage 12.3a: cog-ipc is now a real static library.  It compiles
# src/ipc.c into an archive while preserving the public include path, the
# C11 compile feature, the cog::ipc alias, and the strict-warning settings
# that downstream consumers rely on.
#
# cog-ipc headers include <cog/result.h> from cog-core.  The result
# contract is header-only and exposed as cog::result, so cog-ipc takes the
# cog-core include path and the C11 requirement without linking the
# cog-core archive.  This keeps the dependency graph acyclic: cog-core
# links cog::ipc (for <cog/ipc.h> from event.h) while cog-ipc links only
# the header-only cog::result.  The previous cog::core link produced a
# static-library/export cycle (cog::core -> cog::ipc -> cog::core).
add_library(cog-ipc STATIC
    src/ipc.c
    src/ipc_replay.c
)
add_library(cog::ipc ALIAS cog-ipc)

# Export as cog::ipc (matching the build-tree alias) so installed
# consumers use the same target name as in-tree consumers.
set_target_properties(cog-ipc PROPERTIES EXPORT_NAME ipc)

target_include_directories(cog-ipc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-ipc PUBLIC c_std_11)

# cog-ipc headers include <cog/result.h> (header-only, cog::result).
target_link_libraries(cog-ipc PUBLIC cog::result)

# Apply the project-wide strict-warning policy (and ASan/UBSan when enabled)
# to the production source.
target_enable_warnings(cog-ipc)

# -----------------------------------------------------------------------
# Install rules
# -----------------------------------------------------------------------
include(GNUInstallDirs)

install(DIRECTORY include/cog
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS cog-ipc
    EXPORT cog-ipc-targets)

install(EXPORT cog-ipc-targets
    FILE cog-ipc-targets.cmake
    NAMESPACE cog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog)
