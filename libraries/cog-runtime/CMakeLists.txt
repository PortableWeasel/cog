include(GNUInstallDirs)
add_library(cog-runtime STATIC
 src/instruction_transfer.c
 src/resource_exchange.c
 src/resource_owner.c
 src/resource_proof.c
    src/app_display.c
    src/app_observation.c
    src/app_reload.c
    src/app_sessions.c
    src/app_tools.c
    src/app_turn.c
    src/child_context.c
    src/cli.c
    src/configuration.c
    src/control.c
    src/control_receipt.c
    src/delegation.c
    src/execution.c
    src/handoff.c
    src/host_runtime.c
    src/mock_provider.c
    src/option_values.c
    src/projection.c
    src/projection_codec.c
    src/runtime.c
    src/runtime_instructions.c
    src/worker_host.c
)
add_library(cog::runtime ALIAS cog-runtime)
set_target_properties(cog-runtime PROPERTIES EXPORT_NAME runtime)
target_compile_features(cog-runtime PUBLIC c_std_11)
target_include_directories(cog-runtime PUBLIC
 $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
 $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
 PRIVATE ${PROJECT_SOURCE_DIR}/libraries/cog-ipc/src ${PROJECT_SOURCE_DIR}/libraries/cog-ipc/private)
target_link_libraries(cog-runtime PUBLIC cog::core cog::provider cog::instructions cog::toolkit cog::net cog::obs cog::config cog::orchestrator)
target_enable_warnings(cog-runtime)
install(TARGETS cog-runtime EXPORT cog-runtime-targets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/cog DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT cog-runtime-targets FILE cog-runtime-targets.cmake NAMESPACE cog:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog-runtime)
if(BUILD_TESTING)
 add_executable(test_runtime tests/test_runtime.c)
 target_link_libraries(test_runtime PRIVATE cog::runtime)
 target_enable_warnings(test_runtime)
 add_test(NAME runtime-headless COMMAND test_runtime)
 set_tests_properties(runtime-headless PROPERTIES LABELS "runtime;integration")
endif()

if(BUILD_TESTING)
 foreach(fixture resource_proof instruction_transfer configuration_capsule child_capsule instruction_resolver resource_exchange resource_worker)
  add_executable(test_runtime_${fixture} tests/test_${fixture}.c)
  target_include_directories(test_runtime_${fixture} PRIVATE src)
  target_link_libraries(test_runtime_${fixture} PRIVATE cog::runtime)
  target_enable_warnings(test_runtime_${fixture})
  add_test(NAME runtime-${fixture} COMMAND test_runtime_${fixture})
  set_tests_properties(runtime-${fixture} PROPERTIES LABELS "runtime;contract")
 endforeach()
endif()

if(BUILD_TESTING AND COG_BUILD_CLI)
 find_package(Python3 COMPONENTS Interpreter REQUIRED)
 add_executable(test_native_instructions tests/test_native_instructions.c)
 target_link_libraries(test_native_instructions PRIVATE cog::runtime)
 target_enable_warnings(test_native_instructions)
 add_test(NAME runtime-native-instructions COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_native_instructions.py
  $<TARGET_FILE:test_native_instructions> $<TARGET_FILE:cog-cli> ${PROJECT_BINARY_DIR})
 set_tests_properties(runtime-native-instructions PROPERTIES LABELS "runtime;native;integration" TIMEOUT 120)
endif()

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
 add_executable(test_runtime_ctor_failure tests/test_runtime_ctor_failure.c)
 target_link_libraries(test_runtime_ctor_failure PRIVATE cog::runtime)
 target_link_options(test_runtime_ctor_failure PRIVATE -Wl,--wrap=cog_cli_projection_new)
 target_enable_warnings(test_runtime_ctor_failure)
 add_test(NAME runtime-constructor-failure COMMAND test_runtime_ctor_failure)
 set_tests_properties(runtime-constructor-failure PROPERTIES LABELS "runtime;contract")
endif()
