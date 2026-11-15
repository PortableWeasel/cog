# -----------------------------------------------------------------------
# Unit tests — per-module contract tests (see IMPLEMENTATION_STRATEGY.md)
#
# Each test executable links the cog-core library (cog::core) for its
# public include path, C11 compile feature, and the compiled Phase 1
# modules (session.c, event.c, state.c).  result.h, message.h and
# handler.h are header-only, so their tests exercise the headers through
# the same target.
# -----------------------------------------------------------------------

set(COG_UNIT_TESTS
    test_result
    test_message
    test_session
    test_event
    test_state
    test_handler
    test_core
    test_agent
    test_tool
    test_session_store
)

foreach(test ${COG_UNIT_TESTS})
    add_executable(${test} ${test}.c)
    target_link_libraries(${test} PRIVATE cog::core)
    target_enable_warnings(${test})
endforeach()

# test_core exercises a concurrent cog_core_stop (shutdown must not drop a
# pending message), so it needs the pthread library.
find_package(Threads REQUIRED)
target_link_libraries(test_core PRIVATE Threads::Threads)

# test_agent is a white-box test: it drives the private status-transition
# hooks declared in agent_internal.h (start/complete/fail/timeout), so it
# includes the cog-core src directory.  The public contract is still
# exercised through cog/agent.h.
target_include_directories(test_agent PRIVATE
    ${PROJECT_SOURCE_DIR}/libraries/cog-core/src)

# test_session_store is a white-box test for the store's round-trips: it
# populates raw provider fields and reads private lengths / next-node ids
# through session_internal.h, the same private layout session_store.c
# uses.  The public contract is still exercised through cog/session_store.h.
target_include_directories(test_session_store PRIVATE
    ${PROJECT_SOURCE_DIR}/libraries/cog-core/src)

# -----------------------------------------------------------------------
# CTest registration
# -----------------------------------------------------------------------
add_test(NAME unit-result
    COMMAND test_result
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-message
    COMMAND test_message
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-session
    COMMAND test_session
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-event
    COMMAND test_event
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-state
    COMMAND test_state
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-handler
    COMMAND test_handler
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-core
    COMMAND test_core
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-agent
    COMMAND test_agent
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-tool
    COMMAND test_tool
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-session-store
    COMMAND test_session_store
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# -----------------------------------------------------------------------
# Session append benchmark (opt-in)
#
# bench_session_append is a scaling benchmark, not a timing assertion.
# It is built only when COG_BUILD_BENCHMARKS=ON and is NOT registered with
# CTest, so a normal `ctest` run is never timing-sensitive.  Run it
# directly after building:
#   cmake -S . -B build -DCOG_BUILD_BENCHMARKS=ON
#   cmake --build build --target bench_session_append
#   ./build/tests/unit/bench_session_append
# -----------------------------------------------------------------------
if(COG_BUILD_BENCHMARKS)
    add_executable(bench_session_append bench_session_append.c)
    target_link_libraries(bench_session_append PRIVATE cog::core)
    target_enable_warnings(bench_session_append)
endif()
