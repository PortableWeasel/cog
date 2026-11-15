# -----------------------------------------------------------------------
# cog-ipc — IPC fabric transport layer over Unix domain sockets.
# -----------------------------------------------------------------------

# Stage 6 header-only library.  Implementation files are added in a
# follow-up ticket; for now the target exports the public include path and
# the C11 compile feature so consumers can include the headers.
add_library(cog-ipc INTERFACE)
add_library(cog::ipc ALIAS cog-ipc)

target_include_directories(cog-ipc
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-ipc INTERFACE c_std_11)

# cog-ipc headers include cog-core headers (e.g. <cog/result.h>).
target_link_libraries(cog-ipc INTERFACE cog::core)

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
