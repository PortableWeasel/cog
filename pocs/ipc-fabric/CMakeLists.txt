# -----------------------------------------------------------------------
# PoC 1 — IPC fabric over Unix domain sockets
# -----------------------------------------------------------------------

add_executable(ipc-server
    server.c
    common.c
)

add_executable(ipc-client
    client.c
    common.c
)

add_executable(ipc-edge-test
    edge-test.c
    common.c
)

foreach(target ipc-server ipc-client ipc-edge-test)
    target_include_directories(${target} PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
    target_enable_warnings(${target})
endforeach()

# -----------------------------------------------------------------------
# Smoke test: start the server, run two clients, stop the server.
# -----------------------------------------------------------------------
add_test(NAME ipc-fabric-smoke
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/smoke-test.sh
            "$<TARGET_FILE:ipc-server>"
            "$<TARGET_FILE:ipc-client>"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# -----------------------------------------------------------------------
# Edge-case test: empty payload, max payload, oversized frame.
# -----------------------------------------------------------------------
add_test(NAME ipc-fabric-edge
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/edge-test.sh
            "$<TARGET_FILE:ipc-server>"
            "$<TARGET_FILE:ipc-edge-test>"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
