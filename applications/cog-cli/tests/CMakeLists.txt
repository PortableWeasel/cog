# -----------------------------------------------------------------------
# cog-cli tests
#
# These tests link the cog::cli library and drive the app / mock provider
# programmatically.  They are registered here (not in tests/unit) because
# they depend on the CLI library target.
# -----------------------------------------------------------------------

# Options parsing contract.
add_executable(test_cli_options test_cli_options.c)
target_link_libraries(test_cli_options PRIVATE cog::cli)
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