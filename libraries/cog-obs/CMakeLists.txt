# -----------------------------------------------------------------------
# cog-obs — structured logging, trace spans, and connection diagnostics.
# -----------------------------------------------------------------------

# Stage 12.3b: cog-obs is now a real static library.  It compiles the
# observability modules (obs.c, obs_types.c) into a single archive while
# preserving the public include path, the C11 compile feature, the
# cog::obs alias, and the strict-warning settings that downstream
# consumers rely on.
add_library(cog-obs STATIC
    src/obs.c
    src/obs_types.c
)
add_library(cog::obs ALIAS cog-obs)

target_include_directories(cog-obs
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-obs PUBLIC c_std_11)

# cog-obs headers include cog-core headers (e.g. <cog/result.h>), so the
# core's include path propagates to consumers.  cog-core in turn links
# cog::obs for <cog/obs.h>; the static-library cycle is resolved by CMake
# at link time (verified: cyclic static dependencies are supported).
target_link_libraries(cog-obs PUBLIC cog::core)

# Apply the project-wide strict-warning policy (and ASan/UBSan when enabled)
# to the production sources.
target_enable_warnings(cog-obs)

# -----------------------------------------------------------------------
# Install rules
# -----------------------------------------------------------------------
include(GNUInstallDirs)

install(DIRECTORY include/cog
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS cog-obs
    EXPORT cog-obs-targets)

install(EXPORT cog-obs-targets
    FILE cog-obs-targets.cmake
    NAMESPACE cog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog)
