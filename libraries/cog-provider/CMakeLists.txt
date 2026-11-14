# -----------------------------------------------------------------------
# cog-provider — LLM provider abstraction, endpoint registry, and model
# capability table.
# -----------------------------------------------------------------------

# Stage 6 header-only library.  Implementation files are added in a
# follow-up ticket; for now the target exports the public include path and
# the C11 compile feature so consumers can include the headers.
add_library(cog-provider INTERFACE)
add_library(cog::provider ALIAS cog-provider)

target_include_directories(cog-provider
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-provider INTERFACE c_std_11)

# -----------------------------------------------------------------------
# Install rules
# -----------------------------------------------------------------------
include(GNUInstallDirs)

install(DIRECTORY include/cog
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS cog-provider
    EXPORT cog-provider-targets)

install(EXPORT cog-provider-targets
    FILE cog-provider-targets.cmake
    NAMESPACE cog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog)
