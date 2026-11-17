# Header-first, frontend-neutral instruction contract. No runtime implementation
# is delivered by this INTERFACE target; switch to a compiled library only after
# the header/scenario/review gate. Loaders depend on core neutral data, never UI
# or provider/net implementations.
include(GNUInstallDirs)
add_library(cog-instructions INTERFACE)
add_library(cog::instructions ALIAS cog-instructions)
set_target_properties(cog-instructions PROPERTIES EXPORT_NAME instructions)
target_compile_features(cog-instructions INTERFACE c_std_11)
target_include_directories(cog-instructions INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(cog-instructions INTERFACE cog::result)

install(TARGETS cog-instructions EXPORT cog-instructions-targets)
install(DIRECTORY include/cog DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT cog-instructions-targets FILE cog-instructions-targets.cmake
    NAMESPACE cog:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog-instructions)
