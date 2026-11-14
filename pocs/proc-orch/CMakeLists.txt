# -----------------------------------------------------------------------
# PoC 2 — process orchestration
# -----------------------------------------------------------------------

add_executable(proc-orch
    main.c
    common.c
    orchestrator.c
    worker.c
)

target_include_directories(proc-orch PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_enable_warnings(proc-orch)

# -----------------------------------------------------------------------
# Demo test: run the full scenario and verify every acceptance event.
# -----------------------------------------------------------------------
add_test(NAME proc-orch-demo
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/demo-test.sh
            "$<TARGET_FILE:proc-orch>"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
