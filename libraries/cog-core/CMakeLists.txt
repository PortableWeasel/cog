# -----------------------------------------------------------------------
# cog-core — agent loop, session state, event queue, and state-machine dispatch
# -----------------------------------------------------------------------

# Stage 6 header-only library.  Implementation files are added in a
# follow-up ticket; for now the target exports the public include path and
# the C11 compile feature so consumers can include the headers.
add_library(cog-core INTERFACE)
add_library(cog::core ALIAS cog-core)

target_include_directories(cog-core
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-core INTERFACE c_std_11)

# -----------------------------------------------------------------------
# Install rules
# -----------------------------------------------------------------------
include(GNUInstallDirs)

install(DIRECTORY include/cog
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS cog-core
    EXPORT cog-core-targets)

install(EXPORT cog-core-targets
    FILE cog-core-targets.cmake
    NAMESPACE cog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog)
