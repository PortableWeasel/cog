# -----------------------------------------------------------------------
# Executable scenarios 01-10
#
# Scenarios 11-20 live beside the CLI library and are registered in
# applications/cog-cli/tests/CMakeLists.txt.  This list additionally
# includes the 05b shutdown-cancellation contract regression.
# -----------------------------------------------------------------------

find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

function(cog_add_scenario target ctest_name source)
    add_executable(${target} ${source})
    target_enable_warnings(${target})
    set(native_mode "")
    if(target STREQUAL "test_03_subagent_lifecycle")
        set(native_mode success)
    elseif(target STREQUAL "test_07_aux_crash")
        set(native_mode crash-before)
    elseif(target STREQUAL "test_09_subagent_messaging")
        set(native_mode messaging)
    elseif(target STREQUAL "test_10_fan_out_fan_in")
        set(native_mode fanout)
    endif()
    if(native_mode)
        target_link_libraries(${target} PRIVATE cog-headless-fixture)
        add_dependencies(${target} cog-cli)
        add_test(NAME scenario-${ctest_name} COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_agent_scenarios.py
            $<TARGET_FILE:${target}> $<TARGET_FILE:cog-cli> ${CMAKE_BINARY_DIR} ${native_mode})
        set(native_timeout 90)
    else()
        add_test(NAME scenario-${ctest_name}
            COMMAND ${target}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set(native_timeout 30)
    endif()
    set_tests_properties(scenario-${ctest_name} PROPERTIES
        LABELS "scenario"
        TIMEOUT ${native_timeout}
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
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
target_link_libraries(test_08_config_reload PRIVATE cog::core cog::config cog::cli)

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

# Retain the historical registry/aux/transport proofs as component coverage.
# Canonical O9 tests independently join real installed workers and REAL TLS.
foreach(name IN ITEMS agent_lifecycle aux_crash subagent_messaging fan_out_fan_in)
    add_executable(test_component_${name} components/test_${name}.c)
    target_enable_warnings(test_component_${name})
    target_link_libraries(test_component_${name} PRIVATE cog::cli)
    target_include_directories(test_component_${name} PRIVATE
        ${PROJECT_SOURCE_DIR}/libraries/cog-core/src)
    add_test(NAME component-${name} COMMAND test_component_${name}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(component-${name} PROPERTIES LABELS "component" TIMEOUT 30)
endforeach()
target_link_libraries(test_component_subagent_messaging PRIVATE cog::ipc Threads::Threads)
foreach(name IN ITEMS aux_crash fan_out_fan_in)
    target_link_libraries(test_component_${name} PRIVATE cog::orchestrator Threads::Threads)
    target_include_directories(test_component_${name} PRIVATE
        ${PROJECT_SOURCE_DIR}/libraries/cog-orchestrator/src)
    target_compile_definitions(test_component_${name} PRIVATE
        COG_AUX_CORE_BIN="$<TARGET_FILE:aux_core>")
    add_dependencies(test_component_${name} aux_core)
endforeach()

function(cog_add_native_scenario_mode target mode)
    add_test(NAME scenario-${target}-${mode} COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/run_agent_scenarios.py
        $<TARGET_FILE:${target}> $<TARGET_FILE:cog-cli> ${CMAKE_BINARY_DIR} ${mode})
    set_tests_properties(scenario-${target}-${mode} PROPERTIES
        LABELS "scenario" TIMEOUT 90 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endfunction()
cog_add_native_scenario_mode(test_03_subagent_lifecycle cancel)
cog_add_native_scenario_mode(test_07_aux_crash crash-after)
cog_add_native_scenario_mode(test_10_fan_out_fan_in fanout-cancel)
cog_add_native_scenario_mode(test_10_fan_out_fan_in fanout-limit)
