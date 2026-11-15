# -----------------------------------------------------------------------
# cog-ipc — IPC fabric transport layer over Unix domain sockets.
# -----------------------------------------------------------------------

# Stage 12.3a: cog-ipc is now a real static library.  It compiles
# src/ipc.c into an archive while preserving the public include path, the
# C11 compile feature, the cog::ipc alias, and the strict-warning settings
# that downstream consumers rely on.
#
# cog-ipc headers include cog-core headers (e.g. <cog/result.h>), so
# consumers of cog::ipc must inherit cog-core's include path.  This is a
# header/include dependency, not a symbol dependency: it is linked PUBLIC
# so that the result.h include path propagates to cog::ipc consumers.
add_library(cog-ipc STATIC
    src/ipc.c
)
add_library(cog::ipc ALIAS cog-ipc)

target_include_directories(cog-ipc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-ipc PUBLIC c_std_11)

# cog-ipc headers include cog-core headers (e.g. <cog/result.h>).
target_link_libraries(cog-ipc PUBLIC cog::core)

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
