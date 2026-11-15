# -----------------------------------------------------------------------
# cog-provider — LLM provider abstraction, endpoint registry, and model
# capability table.
# -----------------------------------------------------------------------

# Stage 12.4a: cog-provider is now a real static library.  It compiles
# provider.c and endpoint.c into an archive while preserving the public
# include path, the C11 compile feature, the cog::provider alias, and the
# strict-warning settings that downstream INTERFACE consumers rely on.
add_library(cog-provider STATIC
    src/provider.c
    src/endpoint.c
)
add_library(cog::provider ALIAS cog-provider)

target_include_directories(cog-provider
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-provider PUBLIC c_std_11)

# cog-provider headers include cog-core headers (e.g. <cog/result.h>).
target_link_libraries(cog-provider PUBLIC cog::core)

# Apply the project-wide strict-warning policy (and ASan/UBSan when enabled)
# to the production sources.
target_enable_warnings(cog-provider)

# -----------------------------------------------------------------------
# Unit test — contract test for provider.h / endpoint.h (see
# contracts/CONTRACT.md)
#
# The test is registered here (not in tests/unit/CMakeLists.txt) because it
# links the cog-provider archive and drives real loopback sockets through
# cog-net; the shared wiring in tests/unit/CMakeLists.txt is owned by
# integration ticket 1433731.
# -----------------------------------------------------------------------
add_executable(test_provider
    ${PROJECT_SOURCE_DIR}/tests/unit/test_provider.c
)
target_link_libraries(test_provider PRIVATE cog::provider cog::net)
target_enable_warnings(test_provider)
# The test drives a fake provider that embeds the private provider layout
# (provider_internal.h), so it includes the cog-provider src directory.
target_include_directories(test_provider PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_test(NAME unit-provider
    COMMAND test_provider
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

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