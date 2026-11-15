# -----------------------------------------------------------------------
# cog-config — hierarchical configuration tree with commit/rollback
# -----------------------------------------------------------------------

add_library(cog-config STATIC src/config.c)
add_library(cog::config ALIAS cog-config)

target_include_directories(cog-config
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-config PUBLIC c_std_11)
target_link_libraries(cog-config PUBLIC cog::core)

target_enable_warnings(cog-config)

# Module-owned unit test.
#
# Stage 12.3: this module is self-testable without touching the shared
# tests/unit registration, which integration ticket dd2f55e owns.  When
# dd2f55e consolidates test registration for the four infrastructure
# modules it may move this block into tests/unit/CMakeLists.txt.
add_executable(test_config ${PROJECT_SOURCE_DIR}/tests/unit/test_config.c)
target_link_libraries(test_config PRIVATE cog::config)
target_enable_warnings(test_config)
add_test(NAME unit-config COMMAND test_config
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

include(GNUInstallDirs)

install(DIRECTORY include/cog
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS cog-config
    EXPORT cog-config-targets)

install(EXPORT cog-config-targets
    FILE cog-config-targets.cmake
    NAMESPACE cog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog)
if(BUILD_TESTING)
    add_executable(test_config_bounded tests/test_config_bounded.c src/config.c)
    target_include_directories(test_config_bounded PRIVATE include
        ${PROJECT_SOURCE_DIR}/libraries/cog-core/include)
    target_enable_warnings(test_config_bounded)
    add_test(NAME unit-config-bounded COMMAND test_config_bounded)
endif()
