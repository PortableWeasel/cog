# Draft only: no runtime implementation before the joined review gate.
include(GNUInstallDirs)
add_library(cog-runtime INTERFACE)
add_library(cog::runtime ALIAS cog-runtime)
set_target_properties(cog-runtime PROPERTIES EXPORT_NAME runtime)
target_compile_features(cog-runtime INTERFACE c_std_11)
target_include_directories(cog-runtime INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(cog-runtime INTERFACE cog::core cog::provider cog::instructions)
install(TARGETS cog-runtime EXPORT cog-runtime-targets)
install(DIRECTORY include/cog DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT cog-runtime-targets FILE cog-runtime-targets.cmake
    NAMESPACE cog:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog-runtime)
