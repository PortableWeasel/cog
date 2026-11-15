# -----------------------------------------------------------------------
# Executable scenarios 01-10
#
# Scenarios 11-20 live beside the CLI library and are registered in
# applications/cog-cli/tests/CMakeLists.txt.  This list additionally
# includes the 05b shutdown-cancellation contract regression.
# -----------------------------------------------------------------------

find_package(Threads REQUIRED)

function(cog_add_scenario target ctest_name source)
    add_executable(${target} ${source})
    target_enable_warnings(${target})
    add_test(NAME scenario-${ctest_name}
        COMMAND ${target}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(scenario-${ctest_name} PROPERTIES
        LABELS "scenario"
        TIMEOUT 30)
endfunction()

set(COG_CLI_SCENARIO_TESTS
    test_01_user_prompt
    test_02_network_recovery
    test_03_subagent_lifecycle
    test_04_cold_start
    test_05_graceful_shutdown
    test_05b_shutdown_cancel_contract
)

foreach(test IN LISTS COG_CLI_SCENARIO_TESTS)
    cog_add_scenario(${test} ${test} ${test}.c)
    target_link_libraries(${test} PRIVATE cog::cli)
endforeach()

# Scenario 03 drives private subagent lifecycle transitions through the
# same white-box hooks as its unit tests.
target_include_directories(test_03_subagent_lifecycle PRIVATE
    ${PROJECT_SOURCE_DIR}/libraries/cog-core/src)

cog_add_scenario(test_06_parallel_tools test_06_parallel_tools
    test_06_parallel_tools.c)
target_link_libraries(test_06_parallel_tools PRIVATE
    cog::core cog::toolkit Threads::Threads)

# Scenarios 07 and 10 share the orchestrator's owned aux_core fixture.
# Only PIDs created through those orchestrators are signalled or reaped.
cog_add_scenario(test_07_aux_crash test_07_aux_crash test_07_aux_crash.c)
target_link_libraries(test_07_aux_crash PRIVATE cog::cli cog::orchestrator)
target_include_directories(test_07_aux_crash PRIVATE
    ${PROJECT_SOURCE_DIR}/libraries/cog-core/src
    ${PROJECT_SOURCE_DIR}/libraries/cog-orchestrator/src)
target_compile_definitions(test_07_aux_crash PRIVATE
    COG_AUX_CORE_BIN="$<TARGET_FILE:aux_core>")
add_dependencies(test_07_aux_crash aux_core)

cog_add_scenario(test_08_config_reload test_08_config_reload
    test_08_config_reload.c)
target_link_libraries(test_08_config_reload PRIVATE cog::core cog::config)

cog_add_scenario(test_09_subagent_messaging test_09_subagent_messaging
    test_09_subagent_messaging.c)
target_link_libraries(test_09_subagent_messaging PRIVATE
    cog::cli cog::ipc Threads::Threads)
target_include_directories(test_09_subagent_messaging PRIVATE
    ${PROJECT_SOURCE_DIR}/libraries/cog-core/src)

cog_add_scenario(test_10_fan_out_fan_in test_10_fan_out_fan_in
    test_10_fan_out_fan_in.c)
target_link_libraries(test_10_fan_out_fan_in PRIVATE
    cog::cli cog::orchestrator Threads::Threads)
target_include_directories(test_10_fan_out_fan_in PRIVATE
    ${PROJECT_SOURCE_DIR}/libraries/cog-core/src
    ${PROJECT_SOURCE_DIR}/libraries/cog-orchestrator/src)
target_compile_definitions(test_10_fan_out_fan_in PRIVATE
    COG_AUX_CORE_BIN="$<TARGET_FILE:aux_core>")
add_dependencies(test_10_fan_out_fan_in aux_core)

set_tests_properties(scenario-test_05b_shutdown_cancel_contract PROPERTIES
    LABELS "scenario;regression")
