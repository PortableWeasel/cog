# -----------------------------------------------------------------------
# cog-cli tests
#
# These tests link the cog::cli library and drive the app / mock provider
# programmatically.  They are registered here (not in tests/unit) because
# they depend on the CLI library target.
# -----------------------------------------------------------------------

find_package(Python3 COMPONENTS Interpreter REQUIRED)

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
    set(scenario_timeout 30)
    if(target STREQUAL "test_11_ui_reconnect" OR target STREQUAL "test_17_session_migration")
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        target_link_libraries(${target} PRIVATE cog-headless-fixture)
        add_dependencies(${target} cog-cli)
        if(target STREQUAL "test_11_ui_reconnect")
            set(session_mode reconnect)
            set(scenario_timeout 60)
        else()
            set(session_mode migration)
            set(scenario_timeout 100)
            target_link_options(${target} PRIVATE -Wl,--wrap=cog_orchestrator_copy_event)
        endif()
        add_test(NAME scenario-${target} COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_session_scenarios.py
            $<TARGET_FILE:${target}> $<TARGET_FILE:cog-cli> ${CMAKE_BINARY_DIR} ${session_mode})
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang"
       AND COG_NET_CURL_BUILD_DIR AND COG_NET_CURL_LIBRARY
       AND ("${target}" STREQUAL "test_18_observability" OR "${target}" STREQUAL "test_19_connection_events"))
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME scenario-${target} COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_app_https.py $<TARGET_FILE:${target}> retry)
    else()
        add_test(NAME scenario-${target}
            COMMAND ${target}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    endif()
    set_tests_properties(scenario-${target} PROPERTIES
        LABELS "scenario"
        TIMEOUT ${scenario_timeout}
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
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
target_link_options(test_11_ui_reconnect PRIVATE -Wl,--wrap=cog_ipc_endpoint_send)
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

add_test(NAME scenario-test_17_session_migration-timeout COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_session_scenarios.py
    $<TARGET_FILE:test_17_session_migration> $<TARGET_FILE:cog-cli> ${CMAKE_BINARY_DIR} migration-timeout)
set_tests_properties(scenario-test_17_session_migration-timeout PROPERTIES
    LABELS "scenario" TIMEOUT 60 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

foreach(name IN ITEMS ui_buffer store_transfer)
    add_executable(test_${name}_component components/test_${name}_component.c)
    target_link_libraries(test_${name}_component PRIVATE cog::cli)
    target_enable_warnings(test_${name}_component)
    add_test(NAME component-${name} COMMAND test_${name}_component
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(component-${name} PROPERTIES LABELS "component" TIMEOUT 30)
endforeach()

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

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang"
   AND COG_NET_CURL_BUILD_DIR AND COG_NET_CURL_LIBRARY)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_library(cog-observation-fixture STATIC observation_fixture.c)
    target_include_directories(cog-observation-fixture PRIVATE ../src)
    target_link_libraries(cog-observation-fixture PUBLIC cog::cli)
    target_enable_warnings(cog-observation-fixture)
    add_executable(test_app_observation test_app_observation.c)
    target_enable_warnings(test_app_observation)
    foreach(target test_app_observation test_18_observability test_19_connection_events)
        target_link_libraries(${target} PRIVATE cog-observation-fixture)
        target_link_options(${target} PRIVATE -Wl,--wrap=cog_obs_new -Wl,--wrap=cog_cli_service)
        target_compile_definitions(${target} PRIVATE COG_CLI_OBSERVATION_PROFILE)
    endforeach()
    foreach(mode normal retry stall tls connect cancel)
        add_test(NAME cli-observation-${mode} COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_app_https.py $<TARGET_FILE:test_app_observation> ${mode})
        set_tests_properties(cli-observation-${mode} PROPERTIES TIMEOUT 35)
    endforeach()
    add_executable(test_observation_critic test_observation_critic.c)
    target_include_directories(test_observation_critic PRIVATE ../src)
    target_link_libraries(test_observation_critic PRIVATE cog::cli)
    target_link_options(test_observation_critic PRIVATE -Wl,--wrap=cog_obs_clock_now_ns)
    target_enable_warnings(test_observation_critic)
    add_test(NAME cli-observation-critic COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/run_observation_critic.py $<TARGET_FILE:test_observation_critic>)
    set_tests_properties(cli-observation-critic PROPERTIES TIMEOUT 20)
endif()

add_executable(test_app_loop test_app_loop.c)
target_include_directories(test_app_loop PRIVATE ../src)
target_link_libraries(test_app_loop PRIVATE cog::cli)
target_enable_warnings(test_app_loop)
add_test(NAME cli-app-loop COMMAND test_app_loop)
set_tests_properties(cli-app-loop PROPERTIES TIMEOUT 30)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_executable(test_client_acceptance test_client_acceptance.c)
    target_include_directories(test_client_acceptance PRIVATE ../src)
    target_link_libraries(test_client_acceptance PRIVATE cog::cli)
    target_link_options(test_client_acceptance PRIVATE
        -Wl,--wrap=cog_cli_service -Wl,--wrap=cog_provider_send)
    target_enable_warnings(test_client_acceptance)
    add_test(NAME cli-client-acceptance COMMAND test_client_acceptance)
    set_tests_properties(cli-client-acceptance PROPERTIES TIMEOUT 20)
    add_executable(test_client_ask_output test_client_ask_output.c)
    target_include_directories(test_client_ask_output PRIVATE ../src)
    target_link_libraries(test_client_ask_output PRIVATE cog::cli)
    target_link_options(test_client_ask_output PRIVATE -Wl,--wrap=cog_cli_service)
    target_enable_warnings(test_client_ask_output)
    foreach(mode initial repeat)
        add_test(NAME cli-client-ask-output-${mode} COMMAND test_client_ask_output ${mode})
        set_tests_properties(cli-client-ask-output-${mode} PROPERTIES TIMEOUT 10)
    endforeach()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME cli-client-installed-output COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/run_client_host_output.py ${PROJECT_BINARY_DIR})
    set_tests_properties(cli-client-installed-output PROPERTIES TIMEOUT 30)
    add_executable(test_host_output_critic test_host_output_critic.c ../src/host_output.c)
    target_include_directories(test_host_output_critic PRIVATE ../src)
    target_link_libraries(test_host_output_critic PRIVATE cog::cli)
    target_link_options(test_host_output_critic PRIVATE -Wl,--wrap=write -Wl,--wrap=fopencookie)
    target_enable_warnings(test_host_output_critic)
    add_test(NAME cli-host-output-critic COMMAND test_host_output_critic)
    set_tests_properties(cli-host-output-critic PROPERTIES TIMEOUT 10)
    if(COG_NET_CURL_BUILD_DIR AND COG_NET_CURL_LIBRARY)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_executable(test_client_wire_acceptance test_client_wire_acceptance.c)
        target_include_directories(test_client_wire_acceptance PRIVATE ../src)
        target_link_libraries(test_client_wire_acceptance PRIVATE cog::cli)
        target_link_options(test_client_wire_acceptance PRIVATE
            -Wl,--wrap=cog_cli_service -Wl,--wrap=cog_provider_send -Wl,--wrap=curl_easy_init
            -Wl,--wrap=cog_provider_connect -Wl,--wrap=cog_provider_poll)
        target_enable_warnings(test_client_wire_acceptance)
        foreach(mode fork budget cancel-dns cancel-connect cancel-tls cancel-stream cancel-retained cancel-finish)
            add_test(NAME cli-client-wire-${mode} COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_client_wire_acceptance.py
                $<TARGET_FILE:test_client_wire_acceptance> ${mode})
            set_tests_properties(cli-client-wire-${mode} PROPERTIES TIMEOUT 25)
        endforeach()
    endif()
endif()
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
    target_include_directories(test_app_memory PRIVATE ../src)
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
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_definitions(test_app_reload PRIVATE COG_RELOAD_WRAPPERS)
    target_link_options(test_app_reload PRIVATE
        -Wl,--wrap=cog_cli_service -Wl,--wrap=cog_config_commit
        -Wl,--wrap=cog_provider_connect -Wl,--wrap=cog_provider_disconnect
        -Wl,--wrap=cog_provider_send -Wl,--wrap=cog_provider_preflight -Wl,--wrap=cog_cli_configuration_free)
endif()
if(COG_NET_CURL_BUILD_DIR AND COG_NET_CURL_LIBRARY)
    target_compile_definitions(test_app_reload PRIVATE COG_RELOAD_REAL_PROFILE)
endif()
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

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_executable(test_app_reload_critic test_app_reload_critic.c)
    target_include_directories(test_app_reload_critic PRIVATE ../src)
    target_link_libraries(test_app_reload_critic PRIVATE cog::cli)
    target_link_options(test_app_reload_critic PRIVATE
        -Wl,--wrap=cog_cli_service -Wl,--wrap=cog_provider_connect
        -Wl,--wrap=cog_provider_disconnect)
    target_enable_warnings(test_app_reload_critic)
    add_test(NAME cli-app-reload-critic COMMAND test_app_reload_critic)
    set_tests_properties(cli-app-reload-critic PROPERTIES TIMEOUT 10)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(test_empty_session test_empty_session.c)
    add_dependencies(test_empty_session cog-cli)
    target_include_directories(test_empty_session PRIVATE ../src)
    target_link_libraries(test_empty_session PRIVATE cog::cli)
    target_link_options(test_empty_session PRIVATE
        -Wl,--wrap=cog_store_checkpoint_save -Wl,--wrap=cog_store_owner_compare_exchange
        -Wl,--wrap=cog_store_owner_acquire -Wl,--wrap=cog_store_owner_read)
    target_enable_warnings(test_empty_session)
    add_test(NAME cli-empty-session COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/run_empty_session.py $<TARGET_FILE:test_empty_session>)
    set_tests_properties(cli-empty-session PROPERTIES TIMEOUT 25)
    add_test(NAME cli-installed-empty-session COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/run_installed_empty_session.py
        ${CMAKE_BINARY_DIR} $<TARGET_FILE:test_empty_session>)
    set_tests_properties(cli-installed-empty-session PROPERTIES TIMEOUT 30)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_configuration_clone test_configuration_clone.c)
    target_link_libraries(test_configuration_clone PRIVATE cog-cli-config)
    target_link_options(test_configuration_clone PRIVATE -Wl,--wrap=calloc -Wl,--wrap=free)
    target_enable_warnings(test_configuration_clone)
    add_test(NAME cli-configuration-clone COMMAND test_configuration_clone)
endif()

add_executable(test_execution_headless test_execution_headless.c)
target_include_directories(test_execution_headless PRIVATE ../src)
target_link_libraries(test_execution_headless PRIVATE cog::cli)
target_link_options(test_execution_headless PRIVATE -Wl,--wrap=cog_provider_disconnect)
target_enable_warnings(test_execution_headless)
add_test(NAME cli-execution-headless COMMAND test_execution_headless)

add_executable(test_handoff_leaf test_handoff_leaf.c)
target_include_directories(test_handoff_leaf PRIVATE ../src)
target_link_libraries(test_handoff_leaf PRIVATE cog::cli)
target_enable_warnings(test_handoff_leaf)
add_test(NAME cli-handoff-leaf COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_handoff_leaf.py
    $<TARGET_FILE:test_handoff_leaf> ${CMAKE_BINARY_DIR})
set_tests_properties(cli-handoff-leaf PROPERTIES TIMEOUT 25)

add_test(NAME cli-headless-local COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_headless_local.py
    $<TARGET_FILE:cog-cli> ${CMAKE_BINARY_DIR})
set_tests_properties(cli-headless-local PROPERTIES TIMEOUT 30)

add_executable(test_execution_tools test_execution_tools.c)
target_include_directories(test_execution_tools PRIVATE ../src)
target_link_libraries(test_execution_tools PRIVATE cog::cli)
target_enable_warnings(test_execution_tools)
add_test(NAME cli-execution-tools COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_execution_tools.py
    $<TARGET_FILE:test_execution_tools> ${CMAKE_BINARY_DIR})
set_tests_properties(cli-execution-tools PROPERTIES TIMEOUT 20)

add_executable(test_headless_native test_headless_native.c)
target_include_directories(test_headless_native PRIVATE ../src)
target_link_libraries(test_headless_native PRIVATE cog::cli)
target_enable_warnings(test_headless_native)
add_dependencies(test_headless_native cog-cli)
add_test(NAME cli-headless-native COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_headless_native.py
    $<TARGET_FILE:test_headless_native> $<TARGET_FILE:cog-cli> ${CMAKE_BINARY_DIR})
set_tests_properties(cli-headless-native PROPERTIES TIMEOUT 70)

add_test(NAME cli-headless-native-limit COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_headless_native.py
    $<TARGET_FILE:test_headless_native> $<TARGET_FILE:cog-cli> ${CMAKE_BINARY_DIR} limit)
set_tests_properties(cli-headless-native-limit PROPERTIES TIMEOUT 70)

# Shared O9 support is deliberately test-only; canonical leaves assert their
# own actual runtime joins and keep their provider responders in disjoint files.
add_library(cog-headless-fixture STATIC
    ${PROJECT_SOURCE_DIR}/tests/scenarios/fixtures/headless_fixture.c)
target_include_directories(cog-headless-fixture PUBLIC
    ${PROJECT_SOURCE_DIR}/tests/scenarios/fixtures
    ${PROJECT_SOURCE_DIR}/applications/cog-cli/src)
target_link_libraries(cog-headless-fixture PUBLIC cog::cli)
target_enable_warnings(cog-headless-fixture)
if(MSVC)
    target_compile_options(cog-headless-fixture PRIVATE /UNDEBUG)
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cog-headless-fixture PRIVATE -UNDEBUG)
else()
    message(FATAL_ERROR "Test fixture assertions need an undefine option")
endif()
add_executable(test_headless_fixture_smoke test_headless_fixture_smoke.c)
target_link_libraries(test_headless_fixture_smoke PRIVATE cog-headless-fixture)
target_enable_warnings(test_headless_fixture_smoke)
add_dependencies(test_headless_fixture_smoke cog-cli)
add_test(NAME cli-headless-fixture-smoke COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_headless_native.py
    $<TARGET_FILE:test_headless_fixture_smoke> $<TARGET_FILE:cog-cli> ${CMAKE_BINARY_DIR} single)
set_tests_properties(cli-headless-fixture-smoke PROPERTIES TIMEOUT 70 LABELS "component;headless")
add_test(NAME cli-headless-fixture-python COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_headless_fixture_smoke.py
    $<TARGET_FILE:test_headless_fixture_smoke> $<TARGET_FILE:cog-cli> ${CMAKE_BINARY_DIR})
set_tests_properties(cli-headless-fixture-python PROPERTIES TIMEOUT 70 LABELS "component;headless"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
add_test(NAME cli-headless-fixture-cleanup COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_headless_fixture_cleanup.py ${CMAKE_BINARY_DIR})
set_tests_properties(cli-headless-fixture-cleanup PROPERTIES TIMEOUT 20 LABELS "component;headless"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
# Lane A owns shared CMake; append these real-source private branch fixtures.
foreach(name IN ITEMS worker_receipt_owned delegation_callbacks worker_handoff_critic worker_reload_failure)
    add_executable(test_${name} test_${name}.c)
    target_link_libraries(test_${name} PRIVATE cog::cli)
    target_include_directories(test_${name} PRIVATE ../src ../../../libraries/cog-ipc/private)
    target_compile_options(test_${name} PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_${name} PRIVATE -Wl,--gc-sections)
    target_enable_warnings(test_${name})
endforeach()
add_test(NAME cli-worker-receipt-owned COMMAND test_worker_receipt_owned
    ${CMAKE_BINARY_DIR}/worker-receipt.sock)
add_test(NAME cli-delegation-callbacks COMMAND test_delegation_callbacks)
set_tests_properties(cli-worker-receipt-owned cli-delegation-callbacks PROPERTIES TIMEOUT 10)

add_test(NAME cli-worker-handoff-critic COMMAND test_worker_handoff_critic)
set_tests_properties(cli-worker-handoff-critic PROPERTIES TIMEOUT 10)
# Additive registration for the shared CMake owner. This leaf includes the real
# coordinator translation unit to exercise its private retained-receipt path;
# section GC selects that path, and cog::cli supplies real linked dependencies.
add_executable(test_cli_coordinator_receipt coordinator-leaf/test_receipt_pressure.c)
target_link_libraries(test_cli_coordinator_receipt PRIVATE cog::cli)
target_include_directories(test_cli_coordinator_receipt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(test_cli_coordinator_receipt PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_cli_coordinator_receipt PRIVATE -Wl,--gc-sections)
target_enable_warnings(test_cli_coordinator_receipt)
add_test(NAME cli-coordinator-receipt COMMAND test_cli_coordinator_receipt)
set_tests_properties(cli-coordinator-receipt PROPERTIES LABELS "unit;cli;headless")
add_executable(test_cli_coordinator_identity coordinator-leaf/test_stream_identity_critic.c)
target_link_libraries(test_cli_coordinator_identity PRIVATE cog::cli)
target_include_directories(test_cli_coordinator_identity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(test_cli_coordinator_identity PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_cli_coordinator_identity PRIVATE -Wl,--gc-sections)
target_enable_warnings(test_cli_coordinator_identity)
add_test(NAME cli-coordinator-identity COMMAND test_cli_coordinator_identity)
set_tests_properties(cli-coordinator-identity PROPERTIES LABELS "unit;cli;headless")

add_test(NAME cli-worker-reload-failure COMMAND test_worker_reload_failure)
set_tests_properties(cli-worker-reload-failure PROPERTIES TIMEOUT 10)

add_test(NAME cli-headless-native-single COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_headless_native.py
    $<TARGET_FILE:test_headless_native> $<TARGET_FILE:cog-cli> ${CMAKE_BINARY_DIR} single)
set_tests_properties(cli-headless-native-single PROPERTIES TIMEOUT 70)

add_test(NAME cli-headless-native-handoff COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_headless_native.py
    $<TARGET_FILE:test_headless_native> $<TARGET_FILE:cog-cli> ${CMAKE_BINARY_DIR} handoff)
set_tests_properties(cli-headless-native-handoff PROPERTIES TIMEOUT 145)

add_test(NAME cli-headless-native-cancel COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_headless_native.py
    $<TARGET_FILE:test_headless_native> $<TARGET_FILE:cog-cli> ${CMAKE_BINARY_DIR} cancel)
set_tests_properties(cli-headless-native-cancel PROPERTIES TIMEOUT 70)

add_test(NAME cli-headless-native-handoff-timeout COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_headless_native.py
    $<TARGET_FILE:test_headless_native> $<TARGET_FILE:cog-cli> ${CMAKE_BINARY_DIR} handoff-timeout)
set_tests_properties(cli-headless-native-handoff-timeout PROPERTIES TIMEOUT 145)
# Includes the actual coordinator source. Link real orchestrator and Q implementations.
add_executable(test_coordinator_send_schedule coordinator-leaf/test_send_schedule.c)
target_link_libraries(test_coordinator_send_schedule PRIVATE cog-cli-lib)
target_enable_warnings(test_coordinator_send_schedule)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_coordinator_send_schedule PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(test_coordinator_send_schedule PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME unit-cli-coordinator-send-schedule
  COMMAND test_coordinator_send_schedule coordinator-send.sock)

foreach(name IN ITEMS app_close_guard_critic guard_backend_ownership)
    add_executable(test_${name} test_${name}.c)
    target_include_directories(test_${name} PRIVATE ../src)
    target_link_libraries(test_${name} PRIVATE cog::cli)
    target_enable_warnings(test_${name})
    add_test(NAME cli-${name} COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/run_execution_tools.py
        $<TARGET_FILE:test_${name}> ${CMAKE_BINARY_DIR})
endforeach()
target_link_options(test_app_close_guard_critic PRIVATE -Wl,--wrap=close -Wl,--wrap=cog_store_owner_release)
target_link_options(test_guard_backend_ownership PRIVATE -Wl,--wrap=calloc)

add_executable(test_guard_again_critic test_guard_again_critic.c)
target_include_directories(test_guard_again_critic PRIVATE ../src)
target_link_libraries(test_guard_again_critic PRIVATE cog::cli)
target_enable_warnings(test_guard_again_critic)
target_link_options(test_guard_again_critic PRIVATE -Wl,--wrap=close -Wl,--wrap=cog_store_owner_release)
add_test(NAME cli-guard-again-critic COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_execution_tools.py
    $<TARGET_FILE:test_guard_again_critic> ${CMAKE_BINARY_DIR})

# Maintained O8 protocol/replay oracles now link the actual joined libraries.
# Keep the deliberate fault wrappers local to their owning fixture.
foreach(name IN ITEMS projection_owner client)
    add_executable(test_view_${name} headless-view/test_${name}.c)
    target_include_directories(test_view_${name} PRIVATE ../src)
    target_link_libraries(test_view_${name} PRIVATE cog::cli)
    target_enable_warnings(test_view_${name})
endforeach()
target_link_options(test_view_projection_owner PRIVATE -Wl,--wrap=cog_ipc_replay_append)
target_link_options(test_view_client PRIVATE -Wl,--wrap=clock_gettime)
add_test(NAME cli-view-projection COMMAND test_view_projection_owner)
add_test(NAME cli-view-client COMMAND test_view_client view-client.sock)
set_tests_properties(cli-view-projection cli-view-client PROPERTIES
    TIMEOUT 30 LABELS "unit;cli;headless")

add_executable(test_handoff_release_again_critic test_handoff_release_again_critic.c)
target_include_directories(test_handoff_release_again_critic PRIVATE ../src)
target_link_libraries(test_handoff_release_again_critic PRIVATE cog::cli)
target_enable_warnings(test_handoff_release_again_critic)
target_link_options(test_handoff_release_again_critic PRIVATE
    -Wl,--wrap=close -Wl,--wrap=cog_store_owner_release)
add_test(NAME cli-handoff-release-again-critic COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_execution_tools.py
    $<TARGET_FILE:test_handoff_release_again_critic> ${CMAKE_BINARY_DIR})

add_executable(test_coordinator_begin_failure_critic
    test_coordinator_begin_failure_critic.c ../src/host_output.c)
target_include_directories(test_coordinator_begin_failure_critic PRIVATE ../src)
target_link_libraries(test_coordinator_begin_failure_critic PRIVATE cog::cli)
target_enable_warnings(test_coordinator_begin_failure_critic)
target_link_options(test_coordinator_begin_failure_critic PRIVATE
    -Wl,--wrap=cog_orchestrator_spawn -Wl,--wrap=cog_cli_host_runtime_new
    -Wl,--wrap=cog_cli_host_runtime_close)
add_test(NAME cli-coordinator-begin-failure COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_coordinator_begin_failure.py
    $<TARGET_FILE:test_coordinator_begin_failure_critic> ${CMAKE_BINARY_DIR})
set_tests_properties(cli-coordinator-begin-failure PROPERTIES TIMEOUT 20)
add_executable(test_coordinator_core_begin_failure_critic test_coordinator_core_begin_failure_critic.c)
target_include_directories(test_coordinator_core_begin_failure_critic PRIVATE ../src)
target_link_libraries(test_coordinator_core_begin_failure_critic PRIVATE cog::cli)
target_enable_warnings(test_coordinator_core_begin_failure_critic)
target_link_options(test_coordinator_core_begin_failure_critic PRIVATE -Wl,--wrap=cog_core_drive_begin)
add_dependencies(test_coordinator_core_begin_failure_critic cog-cli)
add_test(NAME cli-coordinator-core-begin-failure COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_coordinator_core_begin_failure.py
    $<TARGET_FILE:test_coordinator_core_begin_failure_critic> $<TARGET_FILE:cog-cli> ${CMAKE_BINARY_DIR})
set_tests_properties(cli-coordinator-core-begin-failure PROPERTIES TIMEOUT 20)
