# Independent instruction reader/catalog/composer; only neutral core contracts.
include(GNUInstallDirs)
add_library(cog-instructions STATIC
    src/util.c src/metadata.c src/catalog.c src/snapshot.c src/restore.c
    src/source.c src/codec.c)
add_library(cog::instructions ALIAS cog-instructions)
set_target_properties(cog-instructions PROPERTIES EXPORT_NAME instructions)
target_compile_features(cog-instructions PUBLIC c_std_11)
target_include_directories(cog-instructions PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(cog-instructions PUBLIC cog::result)
if(COMMAND target_enable_warnings)
    target_enable_warnings(cog-instructions)
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # The hermetic loader consumer has no parent project warning helper.
    target_compile_options(cog-instructions PRIVATE -Wall -Wextra -Wpedantic
        -Wconversion -Wsign-conversion -Wshadow -Wmissing-prototypes
        -Wstrict-prototypes -Werror)
endif()
install(TARGETS cog-instructions EXPORT cog-instructions-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/cog DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT cog-instructions-targets FILE cog-instructions-targets.cmake
    NAMESPACE cog:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog-instructions)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
