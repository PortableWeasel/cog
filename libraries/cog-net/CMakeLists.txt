# -----------------------------------------------------------------------
# cog-net — HTTP client with streaming, stall detection, and retry
# -----------------------------------------------------------------------

add_library(cog-net INTERFACE)
add_library(cog::net ALIAS cog-net)

target_include_directories(cog-net
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-net INTERFACE c_std_11)
target_link_libraries(cog-net INTERFACE cog::core)

include(GNUInstallDirs)

install(DIRECTORY include/cog
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS cog-net
    EXPORT cog-net-targets)

install(EXPORT cog-net-targets
    FILE cog-net-targets.cmake
    NAMESPACE cog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog)