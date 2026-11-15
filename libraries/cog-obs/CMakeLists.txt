# -----------------------------------------------------------------------
# cog-obs — structured logging, trace spans, and connection diagnostics.
# -----------------------------------------------------------------------

# Stage 6 header-only library.  Implementation files are added in a
# follow-up ticket; for now the target exports the public include path and
# the C11 compile feature so consumers can include the headers.
add_library(cog-obs INTERFACE)
add_library(cog::obs ALIAS cog-obs)

target_include_directories(cog-obs
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-obs INTERFACE c_std_11)

# cog-obs headers include cog-core headers (e.g. <cog/result.h>).
target_link_libraries(cog-obs INTERFACE cog::core)

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
