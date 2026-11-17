# -----------------------------------------------------------------------
# cog-provider — LLM provider abstraction, endpoint registry, and model
# capability table.
# -----------------------------------------------------------------------

# Stage 12.4a: cog-provider is now a real static library.  It compiles
# provider.c and endpoint.c into an archive while preserving the public
# include path, the C11 compile feature, the cog::provider alias, and the
# strict-warning settings that downstream INTERFACE consumers rely on.
cog_vendor_yyjson(cog-provider-yyjson provider)
add_library(cog-provider STATIC
    src/provider.c
    src/endpoint.c
    src/openrouter.c
    src/openrouter_codec.c
    $<TARGET_OBJECTS:cog-provider-yyjson>
)
add_library(cog::provider ALIAS cog-provider)

# Export as cog::provider (matching the build-tree alias) so installed
# consumers use the same target name as in-tree consumers.
set_target_properties(cog-provider PROPERTIES EXPORT_NAME provider)

target_include_directories(cog-provider
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-provider PUBLIC c_std_11)

# cog-provider headers include cog-core headers (e.g. <cog/result.h>).
target_link_libraries(cog-provider PUBLIC cog::core cog::net)

# Apply the project-wide strict-warning policy (and ASan/UBSan when enabled)
# to the production sources.
target_enable_warnings(cog-provider)
target_include_directories(cog-provider SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/vendor/yyjson"
    "${PROJECT_SOURCE_DIR}/vendor/yyjson/upstream/src")

# -----------------------------------------------------------------------
# Unit test — contract test for provider.h / endpoint.h (see
# contracts/CONTRACT.md)
#
# The test is registered here (not in tests/unit/CMakeLists.txt) because it
# links the cog-provider archive; actual HTTPS composition is registered below; the shared wiring in tests/unit/CMakeLists.txt is owned by
# integration ticket 1433731.
# -----------------------------------------------------------------------
add_executable(test_provider
    ${PROJECT_SOURCE_DIR}/tests/unit/test_provider.c
)
target_link_libraries(test_provider PRIVATE cog::provider cog::net)
target_enable_warnings(test_provider)
# The legacy include is a compatibility shim; the embedded base is public.
target_include_directories(test_provider PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_test(NAME unit-provider
    COMMAND test_provider
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Critic adversarial contract test (ticket c42b7ca): out-parameter
# preservation, exact vtable call counts, connect/disconnect error
# propagation, and select_by_model / lookup edge cases.
add_executable(test_provider_critic
    ${PROJECT_SOURCE_DIR}/tests/unit/test_provider_critic.c
)
target_link_libraries(test_provider_critic PRIVATE cog::provider)
target_enable_warnings(test_provider_critic)
target_include_directories(test_provider_critic PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_test(NAME unit-provider-critic
    COMMAND test_provider_critic
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
if(BUILD_TESTING)
    add_executable(test_openrouter ${PROJECT_SOURCE_DIR}/tests/unit/test_openrouter.c)
    target_link_libraries(test_openrouter PRIVATE cog::provider)
    target_link_options(test_openrouter PRIVATE
        -Wl,--wrap=cog_net_request,--wrap=cog_net_service,--wrap=cog_net_peek_request
        -Wl,--wrap=cog_net_ack,--wrap=cog_net_request_release
        -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc)
    # Linker wrapping changes allocator effects after compilation. Prevent
    # builtin allocation attributes from hiding the wrappers' counter writes.
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(test_openrouter PRIVATE
            -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc)
    endif()
    target_enable_warnings(test_openrouter)
    add_test(NAME unit-openrouter COMMAND test_openrouter)
endif()
if(BUILD_TESTING)
    # Exercise the mock boundary independently of pending CLI conversation work.
    add_executable(test_mock_provider_contract
        ${PROJECT_SOURCE_DIR}/applications/cog-cli/src/mock_provider.c
        ${PROJECT_SOURCE_DIR}/applications/cog-cli/tests/test_mock_provider.c)
    target_include_directories(test_mock_provider_contract PRIVATE ${PROJECT_SOURCE_DIR}/applications/cog-cli/include)
    target_link_libraries(test_mock_provider_contract PRIVATE cog::provider)
    target_enable_warnings(test_mock_provider_contract)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(test_mock_provider_contract PRIVATE -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc)
        # Allocator linker wrappers invalidate the corresponding builtin effects.
        target_compile_options(test_mock_provider_contract PRIVATE
            -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc)
        target_compile_definitions(test_mock_provider_contract PRIVATE COG_MOCK_WRAP_ALLOC=1)
    endif()
    add_test(NAME unit-mock-provider COMMAND test_mock_provider_contract)
endif()
if(BUILD_TESTING AND COG_NET_CURL_BUILD_DIR AND COG_NET_CURL_LIBRARY)
    add_executable(test_openrouter_https ${PROJECT_SOURCE_DIR}/tests/integration/test_openrouter_https.c)
    target_link_libraries(test_openrouter_https PRIVATE cog::provider)
    target_enable_warnings(test_openrouter_https)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME integration-openrouter-https COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_https.py $<TARGET_FILE:test_openrouter_https>)
endif()

include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/cog-provider-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/cog-provider-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog-provider)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cog-provider-config.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog-provider)
install(FILES "${PROJECT_SOURCE_DIR}/vendor/yyjson/upstream/LICENSE" DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/cog-provider RENAME yyjson-LICENSE)
if(BUILD_TESTING)
    target_compile_options(test_openrouter PRIVATE -UNDEBUG)
endif()

if(BUILD_TESTING)
    add_executable(test_provider_preflight_critic
        ${PROJECT_SOURCE_DIR}/tests/unit/test_provider_preflight_critic.c)
    target_link_libraries(test_provider_preflight_critic PRIVATE cog::provider)
    target_enable_warnings(test_provider_preflight_critic)
    add_test(NAME unit-provider-preflight-critic COMMAND test_provider_preflight_critic)
endif()
