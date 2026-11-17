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
    test_session_codec
    test_session_allocations
    test_event
    test_state
    test_handler
    test_core
    test_agent
    test_tool
    test_session_store
    test_store_adversarial
    test_store_fencing
    test_store_legacy_admission
    test_obs
    test_obs_types
    test_obs_critic
    test_orchestrator
)

foreach(test ${COG_UNIT_TESTS})
    add_executable(${test} ${test}.c)
    target_link_libraries(${test} PRIVATE cog::core)
    target_enable_warnings(${test})
endforeach()

# test_ipc and test_ipc_critic exercise the cog-ipc transport (ipc.c), so
# they link cog::ipc rather than cog::core.  cog::ipc propagates the
# cog-core include path via the header-only cog::result target, so the
# public include path and C11 feature are inherited without linking the
# cog-core archive.  Registration is owned by the integration ticket
# (dd2f55e); the executables were merged with the ipc.c transport.
add_executable(test_ipc test_ipc.c)
target_link_libraries(test_ipc PRIVATE cog::ipc)
target_enable_warnings(test_ipc)

add_executable(test_ipc_critic test_ipc_critic.c)
target_link_libraries(test_ipc_critic PRIVATE cog::ipc)
target_enable_warnings(test_ipc_critic)

# Stage 14.Q registrations are separate from the pending G persistence tests.
# These normal targets link real libraries; the isolated Q script's aborting
# store double is deliberately excluded from CMake/library integration.
add_executable(test_ipc_nonblocking test_ipc_nonblocking.c)
target_link_libraries(test_ipc_nonblocking PRIVATE cog::ipc)
target_enable_warnings(test_ipc_nonblocking)

add_executable(test_core_service test_core_service.c)
target_link_libraries(test_core_service PRIVATE cog::core)
target_enable_warnings(test_core_service)

# test_orchestrator is a white-box test: it drives the private hooks
# declared in orchestrator_internal.h (accept, poll, register_session,
# clock override, timeout overrides), so it includes the cog-orchestrator
# src directory.  The public contract is still exercised through
# cog/orchestrator.h.  It needs pthreads for the shutdown ack responder.
target_link_libraries(test_orchestrator PRIVATE cog::orchestrator Threads::Threads)
target_include_directories(test_orchestrator PRIVATE
    ${PROJECT_SOURCE_DIR}/libraries/cog-orchestrator/src)

# test_core exercises a concurrent cog_core_stop (shutdown must not drop a
# pending message), so it needs the pthread library.
find_package(Threads REQUIRED)
target_link_libraries(test_core PRIVATE Threads::Threads)

# test_core also exercises the stop-after-inject restoration window via a
# test-only linker wrap around cog_event_queue_push (see test_core.c).  The
# wrap is confined to this test binary; production code is unchanged.
target_link_options(test_core PRIVATE -Wl,--wrap=cog_event_queue_push)

# test_agent is a white-box test: it drives the private status-transition
# hooks declared in agent_internal.h (start/complete/fail/timeout), so it
# includes the cog-core src directory.  The public contract is still
# exercised through cog/agent.h.
target_include_directories(test_agent PRIVATE
    ${PROJECT_SOURCE_DIR}/libraries/cog-core/src)

# Private wire codec remains an internal module; the transaction and fencing
# tests use the installed public store/session APIs.
target_include_directories(test_session_codec PRIVATE
    ${PROJECT_SOURCE_DIR}/libraries/cog-core/src)
target_include_directories(test_session_allocations PRIVATE
    ${PROJECT_SOURCE_DIR}/libraries/cog-core/src)
target_link_options(test_session_allocations PRIVATE
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=strdup)
# Allocator linker wrappers invalidate the corresponding builtin effects.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_session_allocations PRIVATE
        -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-strdup)
endif()
target_link_options(test_store_fencing PRIVATE
    -Wl,--wrap=fsync,--wrap=renameat,--wrap=linkat,--wrap=write,--wrap=unlinkat
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=strdup)
target_include_directories(test_store_legacy_admission PRIVATE
    ${PROJECT_SOURCE_DIR}/libraries/cog-core/src)
target_link_options(test_store_legacy_admission PRIVATE -Wl,--wrap=fstatat)
# Allocator linker wrappers invalidate the corresponding builtin effects.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_store_fencing PRIVATE
        -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-strdup)
endif()

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

add_test(NAME unit-session-codec
    COMMAND test_session_codec
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-session-allocations
    COMMAND test_session_allocations
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-store-fencing
    COMMAND test_store_fencing
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME unit-store-legacy-admission COMMAND test_store_legacy_admission)
set_tests_properties(unit-store-legacy-admission PROPERTIES TIMEOUT 20)

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

add_test(NAME unit-store-adversarial
    COMMAND test_store_adversarial
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-obs
    COMMAND test_obs
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-obs-types
    COMMAND test_obs_types
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-obs-critic
    COMMAND test_obs_critic
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-ipc
    COMMAND test_ipc
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-ipc-critic
    COMMAND test_ipc_critic
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-ipc-nonblocking
    COMMAND test_ipc_nonblocking
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-core-service
    COMMAND test_core_service
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME unit-orchestrator
    COMMAND test_orchestrator
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

add_executable(test_ipc_replay test_ipc_replay.c)
target_link_libraries(test_ipc_replay PRIVATE cog::ipc)
target_include_directories(test_ipc_replay PRIVATE ${PROJECT_SOURCE_DIR}/libraries/cog-ipc/src)
target_link_options(test_ipc_replay PRIVATE -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc)
# Allocator linker wrappers invalidate the corresponding builtin effects.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_ipc_replay PRIVATE
        -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc)
endif()
target_enable_warnings(test_ipc_replay)
add_test(NAME unit-ipc-replay COMMAND test_ipc_replay)

# Independent bounded-window model and adversarial CGR1 validation.
add_executable(test_ipc_replay_critic test_ipc_replay_critic.c)
target_link_libraries(test_ipc_replay_critic PRIVATE cog::ipc)
target_link_options(test_ipc_replay_critic PRIVATE -Wl,--wrap=malloc,--wrap=calloc,--wrap=free)
# Allocator linker wrappers invalidate the corresponding builtin effects.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_ipc_replay_critic PRIVATE
        -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-free)
endif()
target_enable_warnings(test_ipc_replay_critic)
add_test(NAME unit-ipc-replay-critic COMMAND test_ipc_replay_critic)

# O1 native provenance and secret-retirement oracles; no production wrapper.
add_executable(test_ipc_authenticated test_ipc_authenticated.c)
target_link_libraries(test_ipc_authenticated PRIVATE cog::ipc)
target_enable_warnings(test_ipc_authenticated)
target_link_options(test_ipc_authenticated PRIVATE
    -Wl,--wrap=getsockopt,--wrap=malloc,--wrap=calloc,--wrap=free,--wrap=send)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_ipc_authenticated PRIVATE
        -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-free)
endif()
add_test(NAME unit-ipc-authenticated COMMAND test_ipc_authenticated)
set_tests_properties(unit-ipc-authenticated PROPERTIES LABELS "unit;ipc" TIMEOUT 20)

# Independent O1 prequeued-proof, wrapped-RX retirement and isolated-TX joins.
add_executable(test_ipc_o1_review test_ipc_o1_review.c)
target_link_libraries(test_ipc_o1_review PRIVATE cog::ipc)
target_enable_warnings(test_ipc_o1_review)
target_link_options(test_ipc_o1_review PRIVATE
    -Wl,--wrap=getsockopt,--wrap=recv,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_ipc_o1_review PRIVATE
        -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free)
endif()
add_test(NAME unit-ipc-o1-review COMMAND test_ipc_o1_review)
set_tests_properties(unit-ipc-o1-review PROPERTIES LABELS "unit;ipc" TIMEOUT 20)

# Private read-only work query needed by cooperative core driving (5fcaef7).
add_executable(test_ipc_readiness test_ipc_readiness.c)
target_link_libraries(test_ipc_readiness PRIVATE cog::ipc)
target_enable_warnings(test_ipc_readiness)
target_link_options(test_ipc_readiness PRIVATE -Wl,--wrap=poll,--wrap=connect,--wrap=send)
add_test(NAME unit-ipc-readiness COMMAND test_ipc_readiness)
set_tests_properties(unit-ipc-readiness PROPERTIES LABELS "unit;ipc" TIMEOUT 20)

# O2 manual/run engine equivalence and registry execution-session lifetime.
add_executable(test_core_cooperative test_core_cooperative.c)
target_link_libraries(test_core_cooperative PRIVATE cog::core)
target_enable_warnings(test_core_cooperative)
target_link_options(test_core_cooperative PRIVATE -Wl,--wrap=clock_gettime)
add_test(NAME unit-core-cooperative COMMAND test_core_cooperative)
set_tests_properties(unit-core-cooperative PROPERTIES LABELS "unit;core" TIMEOUT 20)

# O2 captures real kernel proof before further I/O despite queue/arena pressure.
add_executable(test_core_proof test_core_proof.c)
target_link_libraries(test_core_proof PRIVATE cog::core)
target_enable_warnings(test_core_proof)
target_link_options(test_core_proof PRIVATE
    -Wl,--wrap=cog_ipc_endpoint_peer_info,--wrap=getsockopt)
add_test(NAME unit-core-proof COMMAND test_core_proof)
set_tests_properties(unit-core-proof PROPERTIES LABELS "unit;core;ipc" TIMEOUT 20)

# O2 allocation fault/positive controls use real libraries and keep optimizer oracles.
add_executable(test_core_reservations test_core_reservations.c)
target_link_libraries(test_core_reservations PRIVATE cog::core)
target_enable_warnings(test_core_reservations)
target_link_options(test_core_reservations PRIVATE -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_core_reservations PRIVATE
        -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc)
endif()
add_test(NAME unit-core-reservations COMMAND test_core_reservations)
set_tests_properties(unit-core-reservations PROPERTIES LABELS "unit;core" TIMEOUT 20)

# O3 allocator failure/steady-state oracle covers real constructors and runtime.
target_link_options(test_orchestrator PRIVATE -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_orchestrator PRIVATE
        -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc)
endif()
add_executable(test_orchestrator_wire ../orchestrator-wire/test_orchestrator_wire.c)
target_link_libraries(test_orchestrator_wire PRIVATE cog::orchestrator)
target_enable_warnings(test_orchestrator_wire)
target_link_options(test_orchestrator_wire PRIVATE -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free)
add_test(NAME unit-orchestrator-wire COMMAND test_orchestrator_wire)
set_tests_properties(unit-orchestrator-wire PROPERTIES LABELS "unit;orchestrator" TIMEOUT 20)
