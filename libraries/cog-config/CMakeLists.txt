# -----------------------------------------------------------------------
# cog-config — hierarchical configuration tree with commit/rollback
# -----------------------------------------------------------------------

add_library(cog-config INTERFACE)
add_library(cog::config ALIAS cog-config)

target_include_directories(cog-config
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-config INTERFACE c_std_11)
target_link_libraries(cog-config INTERFACE cog::core)

include(GNUInstallDirs)

install(DIRECTORY include/cog
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS cog-config
    EXPORT cog-config-targets)

install(EXPORT cog-config-targets
    FILE cog-config-targets.cmake
    NAMESPACE cog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog)