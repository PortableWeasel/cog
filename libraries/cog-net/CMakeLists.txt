# -----------------------------------------------------------------------
# cog-net — HTTP client with streaming, stall detection, and retry
# -----------------------------------------------------------------------

# Stage 12.3d: cog-net is now a real static library.  It compiles net.c
# into an archive while preserving the public include path, the C11
# compile feature, the cog::net alias, and the strict-warning settings
# that downstream INTERFACE consumers rely on.
add_library(cog-net STATIC
    src/net.c
)
add_library(cog::net ALIAS cog-net)

target_include_directories(cog-net
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-net PUBLIC c_std_11)
target_link_libraries(cog-net PUBLIC cog::core)

# Apply the project-wide strict-warning policy (and ASan/UBSan when enabled)
# to the production sources.
target_enable_warnings(cog-net)

# -----------------------------------------------------------------------
# Unit test — contract test for net.h (see contracts/CONTRACT.md)
#
# The test is registered here (not in tests/unit/CMakeLists.txt) because
# it links the cog-net archive and drives real loopback sockets; the
# shared wiring in tests/unit/CMakeLists.txt is owned by integration
# ticket dd2f55e.
# -----------------------------------------------------------------------
add_executable(test_net
    ${PROJECT_SOURCE_DIR}/tests/unit/test_net.c
)
target_link_libraries(test_net PRIVATE cog::net)
target_enable_warnings(test_net)

add_test(NAME unit-net
    COMMAND test_net
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Adversarial contract tests (review ffb1ae0): EOF mid-frame, malformed
# framing, slow-head stall semantics, FIN survival, multi-request ids,
# slot reuse, and non-retried 4xx/429 surfacing.
add_executable(test_net_adversarial
    ${PROJECT_SOURCE_DIR}/tests/unit/test_net_adversarial.c
)
target_link_libraries(test_net_adversarial PRIVATE cog::net)
target_enable_warnings(test_net_adversarial)

add_test(NAME unit-net-adversarial
    COMMAND test_net_adversarial
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# -----------------------------------------------------------------------
# Install rules
# -----------------------------------------------------------------------
include(GNUInstallDirs)

install(DIRECTORY include/cog
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS cog-net
    EXPORT cog-net-targets)

install(EXPORT cog-net-targets
    FILE cog-net-targets.cmake
    NAMESPACE cog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog)