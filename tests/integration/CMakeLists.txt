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

add_executable(test_core_session
    test_core_session.c
)

target_link_libraries(test_core_session PRIVATE cog::core)
target_enable_warnings(test_core_session)

add_executable(test_store_session
    test_store_session.c
)

target_link_libraries(test_store_session PRIVATE cog::core)
target_enable_warnings(test_store_session)

# test_core_ipc wires the core run loop to a real cog-ipc endpoint over a
# local Unix domain socket.  Peers run on helper threads because the IPC
# transport is blocking, so it needs pthreads.
add_executable(test_core_ipc
    test_core_ipc.c
)

target_link_libraries(test_core_ipc PRIVATE cog::core)
target_enable_warnings(test_core_ipc)

# test_core_obs configures a real cog-obs context on a core and proves the
# core emits lifecycle/error records through it.
add_executable(test_core_obs
    test_core_obs.c
)

target_link_libraries(test_core_obs PRIVATE cog::core)
target_enable_warnings(test_core_obs)

find_package(Threads REQUIRED)
target_link_libraries(test_core_ipc PRIVATE Threads::Threads)

# test_core_provider wires a real cog-core agent to a fake static-vtable
# provider and drives a user-input -> provider-send -> LLM_CHUNK ->
# LLM_COMPLETE flow through real dispatch and session history, covering
# streaming content, error/retry metadata propagation, cancellation, and
# provider accessor/ownership.
add_executable(test_core_provider
    test_core_provider.c
)

target_link_libraries(test_core_provider PRIVATE cog::core cog::provider)
target_enable_warnings(test_core_provider)

# test_core_toolkit wires a real core/static tool table to the toolkit
# dispatcher and proves bounded parallel execution (overlap), ordered
# result collection into session history, timeout, cooperative
# cancellation, and tool error propagation.
add_executable(test_core_toolkit
    test_core_toolkit.c
)

target_link_libraries(test_core_toolkit PRIVATE cog::core cog::toolkit Threads::Threads)
target_enable_warnings(test_core_toolkit)

# -----------------------------------------------------------------------
# CTest registration
# -----------------------------------------------------------------------
add_test(NAME integration-core-primitives
    COMMAND test_core_primitives
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME integration-core-session
    COMMAND test_core_session
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME integration-store-session
    COMMAND test_store_session
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME integration-core-ipc
    COMMAND test_core_ipc
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME integration-core-obs
    COMMAND test_core_obs
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME integration-core-provider
    COMMAND test_core_provider
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME integration-core-toolkit
    COMMAND test_core_toolkit
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
