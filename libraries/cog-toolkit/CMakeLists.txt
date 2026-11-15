# -----------------------------------------------------------------------
# cog-toolkit — tool execution framework
# -----------------------------------------------------------------------

# Stage 6 header-only library.  Implementation files are added in a
# follow-up ticket; for now the target exports the public include path and
# the C11 compile feature so consumers can include the headers.
add_library(cog-toolkit INTERFACE)
add_library(cog::toolkit ALIAS cog-toolkit)

target_include_directories(cog-toolkit
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-toolkit INTERFACE c_std_11)

target_link_libraries(cog-toolkit INTERFACE cog::core)

# -----------------------------------------------------------------------
# Install rules
# -----------------------------------------------------------------------
include(GNUInstallDirs)

install(DIRECTORY include/cog
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS cog-toolkit
    EXPORT cog-toolkit-targets)

install(EXPORT cog-toolkit-targets
    FILE cog-toolkit-targets.cmake
    NAMESPACE cog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog)
