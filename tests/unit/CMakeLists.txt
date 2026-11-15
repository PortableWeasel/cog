# -----------------------------------------------------------------------
# Unit tests — per-module contract tests (see IMPLEMENTATION_STRATEGY.md)
#
# Phase 0: result.h and message.h are header-only, so the test
# executables link the cog-core INTERFACE target purely for its public
# include path and C11 compile feature; no production sources exist yet.
# -----------------------------------------------------------------------

add_executable(test_result
    test_result.c
)

add_executable(test_message
    test_message.c
)

foreach(target test_result test_message)
    target_link_libraries(${target} PRIVATE cog::core)
    target_enable_warnings(${target})
endforeach()

# -----------------------------------------------------------------------
# CTest registration
# -----------------------------------------------------------------------
add_test(NAME unit-result
    COMMAND test_result
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-message
    COMMAND test_message
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
