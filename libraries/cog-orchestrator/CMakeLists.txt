# -----------------------------------------------------------------------
# cog-orchestrator — process lifecycle, health tracking, placement
# -----------------------------------------------------------------------

add_library(cog-orchestrator INTERFACE)
add_library(cog::orchestrator ALIAS cog-orchestrator)

target_include_directories(cog-orchestrator
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-orchestrator INTERFACE c_std_11)
target_link_libraries(cog-orchestrator INTERFACE cog::core cog::ipc)

include(GNUInstallDirs)

install(DIRECTORY include/cog
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS cog-orchestrator
    EXPORT cog-orchestrator-targets)

install(EXPORT cog-orchestrator-targets
    FILE cog-orchestrator-targets.cmake
    NAMESPACE cog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog)