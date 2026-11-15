# -----------------------------------------------------------------------
# Integration tests — cross-module paths through the compiled cog-core
# library (session.c + event.c + state.c + header-only handler.h).
#
# These tests link the real cog-core archive and exercise the primitives
# together, without implementing core.c or any later-phase behaviour.
# -----------------------------------------------------------------------

add_executable(test_core_primitives
    test_core_primitives.c
)

target_link_libraries(test_core_primitives PRIVATE cog::core)
target_enable_warnings(test_core_primitives)

# -----------------------------------------------------------------------
# CTest registration
# -----------------------------------------------------------------------
add_test(NAME integration-core-primitives
    COMMAND test_core_primitives
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
