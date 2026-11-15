# -----------------------------------------------------------------------
# cog-cli tests
#
# These tests link the cog::cli library and drive the app / mock provider
# programmatically.  They are registered here (not in tests/unit) because
# they depend on the CLI library target.
# -----------------------------------------------------------------------

# Options parsing contract.
add_executable(test_cli_options test_cli_options.c)
target_link_libraries(test_cli_options PRIVATE cog-cli-config)
target_enable_warnings(test_cli_options)

# Mock provider contract (chunks, errors, cancellation, rate limits,
# scripted behaviour, observable state).
add_executable(test_mock_provider test_mock_provider.c)
target_link_libraries(test_mock_provider PRIVATE cog::cli)
target_enable_warnings(test_mock_provider)

# End-to-end app flows through the real core/session/provider/obs/store:
# streaming, queueing, cancellation, error+retry, shutdown, persistence.
add_executable(test_cli_smoke test_cli_smoke.c)
target_link_libraries(test_cli_smoke PRIVATE cog::cli)
target_enable_warnings(test_cli_smoke)

# Scenario-harness contract: the exact fixture pattern scenario writers
# (Stages 12.6b–12.6e) use to inject prompts/chunks/errors and observe
# session/event/provider state.
add_executable(test_cli_harness test_cli_harness.c)
target_link_libraries(test_cli_harness PRIVATE cog::cli)
target_enable_warnings(test_cli_harness)

# Critic regression tests: non-retryable error propagation, NULL handling,
# and store auto-save on complete-vs-cancel.
add_executable(test_cli_regression test_cli_regression.c)
target_link_libraries(test_cli_regression PRIVATE cog::cli)
target_enable_warnings(test_cli_regression)

# Spawns the built cog-cli binary and checks its stdout/exit code for the
# stream, error-retry, and cancel mock modes.
add_executable(run_cli_smoke run_cli_smoke.c)
target_link_libraries(run_cli_smoke PRIVATE cog::cli)
target_enable_warnings(run_cli_smoke)
target_compile_definitions(run_cli_smoke PRIVATE
    COG_CLI_BIN="$<TARGET_FILE:cog-cli>")

# -----------------------------------------------------------------------
# Executable scenarios 11-20
#
# Scenarios 01-10 are registered from tests/scenarios.  Keeping the later
# scenarios beside the CLI library preserves their source ownership while
# the common scenario label makes the complete set selectable in CTest.
# -----------------------------------------------------------------------
function(cog_add_cli_scenario target)
    add_executable(${target} ${target}.c)
    target_enable_warnings(${target})
    add_test(NAME scenario-${target}
        COMMAND ${target}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(scenario-${target} PROPERTIES
        LABELS "scenario"
        TIMEOUT 30)
endfunction()

set(COG_APP_SCENARIO_TESTS
    test_11_ui_reconnect
    test_12_rate_limit
    test_13_session_persistence
    test_14_conversation_fork
    test_15_multi_turn_tools
    test_16_error_propagation
    test_17_session_migration
    test_18_observability
    test_19_connection_events
    test_20_interrupt_and_queue
)

foreach(test IN LISTS COG_APP_SCENARIO_TESTS)
    cog_add_cli_scenario(${test})
endforeach()

target_link_libraries(test_11_ui_reconnect PRIVATE cog::cli)
target_link_libraries(test_12_rate_limit PRIVATE cog::cli)
target_link_libraries(test_13_session_persistence PRIVATE cog::core)
target_link_libraries(test_14_conversation_fork PRIVATE cog::core)
target_link_libraries(test_15_multi_turn_tools PRIVATE cog::cli cog::toolkit)
target_link_libraries(test_16_error_propagation PRIVATE cog::cli)
target_link_libraries(test_17_session_migration PRIVATE cog::cli)
target_link_libraries(test_18_observability PRIVATE cog::obs)
target_link_libraries(test_19_connection_events PRIVATE cog::obs)
target_link_libraries(test_20_interrupt_and_queue PRIVATE cog::cli)

# -----------------------------------------------------------------------
# CTest registration
# -----------------------------------------------------------------------
add_test(NAME cli-options
    COMMAND test_cli_options
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME cli-mock-provider
    COMMAND test_mock_provider
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME cli-smoke
    COMMAND test_cli_smoke
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME cli-harness
    COMMAND test_cli_harness
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME cli-regression
    COMMAND test_cli_regression
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_test(NAME cli-binary-smoke
    COMMAND run_cli_smoke
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
# J resolution contract, independent of provider/store/app implementation migration.
add_executable(test_configuration test_configuration.c)
target_link_libraries(test_configuration PRIVATE cog-cli-config)
target_enable_warnings(test_configuration)
add_test(NAME cli-configuration COMMAND test_configuration)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_executable(test_configuration_memory test_configuration_memory.c
        ${PROJECT_SOURCE_DIR}/libraries/cog-config/src/config.c)
    target_include_directories(test_configuration_memory PRIVATE
        ${PROJECT_SOURCE_DIR}/libraries/cog-config/include)
    target_link_libraries(test_configuration_memory PRIVATE cog-cli-config)
    target_link_options(test_configuration_memory PRIVATE
        -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=free)
    target_enable_warnings(test_configuration_memory)
    add_test(NAME cli-configuration-memory COMMAND test_configuration_memory)
endif()
