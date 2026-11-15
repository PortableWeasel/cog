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
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(test_mock_provider PRIVATE -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc)
    # Allocator linker wrappers invalidate the corresponding builtin effects.
    target_compile_options(test_mock_provider PRIVATE
        -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc)
    target_compile_definitions(test_mock_provider PRIVATE COG_MOCK_WRAP_ALLOC=1)
endif()

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
target_include_directories(test_15_multi_turn_tools PRIVATE ../src)
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
    # Allocator linker wrappers invalidate the corresponding builtin effects.
    target_compile_options(test_configuration_memory PRIVATE
        -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-free)
    target_enable_warnings(test_configuration_memory)
    add_test(NAME cli-configuration-memory COMMAND test_configuration_memory)
endif()

add_executable(test_app_loop test_app_loop.c)
target_include_directories(test_app_loop PRIVATE ../src)
target_link_libraries(test_app_loop PRIVATE cog::cli)
target_enable_warnings(test_app_loop)
add_test(NAME cli-app-loop COMMAND test_app_loop)
set_tests_properties(cli-app-loop PROPERTIES TIMEOUT 30)
if(COG_NET_CURL_BUILD_DIR AND COG_NET_CURL_LIBRARY)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(test_app_https test_app_https.c)
    target_link_libraries(test_app_https PRIVATE cog::cli)
    target_enable_warnings(test_app_https)
    add_test(NAME cli-app-https COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/run_app_https.py $<TARGET_FILE:test_app_https>)
    set_tests_properties(cli-app-https PROPERTIES TIMEOUT 30)
endif()
find_package(Threads REQUIRED)
add_executable(test_app_stop test_app_stop.c)
target_link_libraries(test_app_stop PRIVATE cog::cli Threads::Threads)
target_enable_warnings(test_app_stop)
add_test(NAME cli-app-stop COMMAND test_app_stop)
set_tests_properties(cli-app-stop PROPERTIES TIMEOUT 10)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_executable(test_app_memory test_app_memory.c)
    target_link_libraries(test_app_memory PRIVATE cog::cli)
    target_link_options(test_app_memory PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc)
    # Allocator linker wrappers invalidate the corresponding builtin effects.
    target_compile_options(test_app_memory PRIVATE
        -fno-builtin-malloc -fno-builtin-calloc)
    target_enable_warnings(test_app_memory)
    add_test(NAME cli-app-memory COMMAND test_app_memory)
    add_executable(test_app_save test_app_save.c)
    target_link_libraries(test_app_save PRIVATE cog::cli)
    target_link_options(test_app_save PRIVATE -Wl,--wrap=cog_store_save -Wl,--wrap=cog_session_append)
    target_include_directories(test_app_save PRIVATE ../src)
    target_enable_warnings(test_app_save)
    add_test(NAME cli-app-save COMMAND test_app_save)
endif()
add_executable(test_app_limits test_app_limits.c)
target_link_libraries(test_app_limits PRIVATE cog::cli Threads::Threads)
target_enable_warnings(test_app_limits)
add_test(NAME cli-app-limits COMMAND test_app_limits)
set_tests_properties(cli-app-limits PROPERTIES TIMEOUT 30)
add_executable(test_app_codec_critic test_app_codec_critic.c)
target_link_libraries(test_app_codec_critic PRIVATE cog::cli)
target_enable_warnings(test_app_codec_critic)
add_test(NAME cli-app-codec-critic COMMAND test_app_codec_critic)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME cli-host-signal COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_host_signal.py $<TARGET_FILE:cog-cli>)
set_tests_properties(cli-host-signal PROPERTIES TIMEOUT 15)

add_executable(test_session_ux test_session_ux.c)
target_include_directories(test_session_ux PRIVATE ../src)
target_link_libraries(test_session_ux PRIVATE cog::cli)
if(COG_NET_CURL_BUILD_DIR AND COG_NET_CURL_LIBRARY)
    target_compile_definitions(test_session_ux PRIVATE COG_SESSION_TEST_REAL_PROFILE)
endif()
target_enable_warnings(test_session_ux)
add_test(NAME cli-session-ux COMMAND test_session_ux)

add_executable(test_app_reload test_app_reload.c)
target_include_directories(test_app_reload PRIVATE ../src)
target_link_libraries(test_app_reload PRIVATE cog::cli)
target_enable_warnings(test_app_reload)
add_test(NAME cli-app-reload COMMAND test_app_reload)
set_tests_properties(cli-app-reload PROPERTIES TIMEOUT 30)
set_tests_properties(cli-session-ux PROPERTIES TIMEOUT 30)
add_test(NAME cli-installed-session-ux COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_installed_session_ux.py ${PROJECT_BINARY_DIR}
    $<TARGET_FILE:test_session_ux>)
set_tests_properties(cli-installed-session-ux PROPERTIES TIMEOUT 45)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_executable(test_session_recovery_faults test_session_recovery_faults.c)
    target_include_directories(test_session_recovery_faults PRIVATE ../src)
    target_link_libraries(test_session_recovery_faults PRIVATE cog::cli)
    target_link_options(test_session_recovery_faults PRIVATE -Wl,--wrap=cog_store_save)
    target_enable_warnings(test_session_recovery_faults)
    add_test(NAME cli-session-recovery-faults COMMAND test_session_recovery_faults)
    set_tests_properties(cli-session-recovery-faults PROPERTIES TIMEOUT 20)
endif()
