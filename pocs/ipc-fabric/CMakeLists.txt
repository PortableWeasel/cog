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

foreach(target ipc-server ipc-client)
    target_include_directories(${target} PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
    target_enable_warnings(${target})
endforeach()

# -----------------------------------------------------------------------
# Smoke test: start the server, run two clients, stop the server.
# -----------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(SOCAT_EXECUTABLE socat)
endif()

add_test(NAME ipc-fabric-smoke
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/smoke-test.sh
            "$<TARGET_FILE:ipc-server>"
            "$<TARGET_FILE:ipc-client>"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
