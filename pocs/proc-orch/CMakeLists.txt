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
# Regression test: feeding a max-size frame through the reader must not
# write past the payload buffer (ASAN/UBSan would flag the overflow).
# -----------------------------------------------------------------------
add_executable(reader-overflow-test
    reader-overflow-test.c
    common.c
)
target_include_directories(reader-overflow-test PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_enable_warnings(reader-overflow-test)
add_test(NAME proc-orch-reader-overflow
    COMMAND reader-overflow-test)

# -----------------------------------------------------------------------
# Demo test: run the full scenario and verify every acceptance event.
# -----------------------------------------------------------------------
add_test(NAME proc-orch-demo
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/demo-test.sh
            "$<TARGET_FILE:proc-orch>"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
