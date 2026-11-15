# Define helpers before target-owning subdirectories; finalize the inventory
# after all targets exist. PRIVATE options affect test executables only.
function(cog_enable_test_assertions target)
    get_target_property(type ${target} TYPE)
    if(NOT type STREQUAL "EXECUTABLE")
        message(FATAL_ERROR "Test assertion policy requires an executable: ${target}")
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /UNDEBUG)
    elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -UNDEBUG)
    else()
        message(FATAL_ERROR "Test assertions need an undefine option for ${CMAKE_C_COMPILER_ID}")
    endif()
    set_property(TARGET ${target} PROPERTY COG_TEST_ASSERTIONS TRUE)
endfunction()

function(cog_collect_test_assertions directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(NOT type STREQUAL "EXECUTABLE")
            continue()
        endif()
        set_property(GLOBAL APPEND PROPERTY COG_PROJECT_EXECUTABLES ${target})
        # Current library, application and standalone test naming convention.
        # The generated CTest audit rejects new registered executables that do
        # not match it: explicitly opt them in with cog_enable_test_assertions.
        if(target MATCHES "^test_" OR target STREQUAL "run_cli_smoke" OR
           target STREQUAL "aux_core" OR target IN_LIST COG_POC_TEST_EXECUTABLES)
            cog_enable_test_assertions(${target})
        endif()
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        cog_collect_test_assertions("${child}")
    endforeach()
endfunction()

function(cog_configure_test_assertions)
    # These opt-in PoCs are executable tests; their ordinary demo/helper
    # programs below remain representative of production assertion settings.
    set(COG_POC_TEST_EXECUTABLES config-tree-test http-unit-test http-net-test
        ipc-edge-test reader-overflow-test provider-wire https-lifecycle)
    set(production_executables cog-cli config-tree-demo http-mock-server
        http-demo ipc-server ipc-client proc-orch)
    cog_collect_test_assertions("${PROJECT_SOURCE_DIR}")
    get_property(targets GLOBAL PROPERTY COG_PROJECT_EXECUTABLES)
    set(manifest "")
    foreach(target IN LISTS targets)
        string(APPEND manifest "list(APPEND COG_PROJECT_EXECUTABLES [==[$<TARGET_FILE:${target}>]==])\n")
        get_target_property(enabled ${target} COG_TEST_ASSERTIONS)
        if(enabled)
            string(APPEND manifest "list(APPEND COG_ASSERTION_EXECUTABLES [==[$<TARGET_FILE:${target}>]==])\n")
        elseif(target IN_LIST production_executables)
            string(APPEND manifest "list(APPEND COG_PRODUCTION_EXECUTABLES [==[$<TARGET_FILE:${target}>]==])\n")
        endif()
    endforeach()
    file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/test-assertions-$<CONFIG>.cmake"
        CONTENT "${manifest}")
endfunction()
