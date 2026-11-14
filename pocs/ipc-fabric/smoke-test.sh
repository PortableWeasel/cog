#!/bin/sh
# Smoke test for the IPC fabric PoC.
#
# Usage: smoke-test.sh <path/to/ipc-server> <path/to/ipc-client>

set -u

SERVER="$1"
CLIENT="$2"
SOCKET="${COG_IPC_SOCKET:-/tmp/cog-ipc-poc-smoke.sock}"

export COG_IPC_SOCKET="${SOCKET}"

# Clean up any leftover socket from a previous run.
rm -f "${SOCKET}"

# Start the server in the background.
"${SERVER}" > server.log 2>&1 &
SERVER_PID=$!

# Ensure the server is stopped on exit.
cleanup() {
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
    rm -f "${SOCKET}"
}
trap cleanup EXIT

# Wait for the socket to appear (timeout after 5 seconds).
for _ in $(seq 1 50); do
    if [ -S "${SOCKET}" ]; then
        break
    fi
    sleep 0.1
done

if [ ! -S "${SOCKET}" ]; then
    echo "server did not create socket" >&2
    cat server.log >&2 || true
    exit 1
fi

# Run two clients concurrently.
"${CLIENT}" > client1.log 2>&1 &
CLIENT1_PID=$!
"${CLIENT}" > client2.log 2>&1 &
CLIENT2_PID=$!

CLIENT1_STATUS=0
CLIENT2_STATUS=0
wait "${CLIENT1_PID}" || CLIENT1_STATUS=$?
wait "${CLIENT2_PID}" || CLIENT2_STATUS=$?

# Verify each client saw replies for every message sent.
SMOKE_STATUS=0
for f in client1.log client2.log; do
    count=$(grep -c 'hello from cycle' "${f}" 2>/dev/null || true)
    if [ "${count}" -ne 6 ]; then
        echo "${f}: expected 6 replies, found ${count}" >&2
        cat "${f}" >&2 || true
        SMOKE_STATUS=1
    fi
done

if [ "${CLIENT1_STATUS}" -ne 0 ] || [ "${CLIENT2_STATUS}" -ne 0 ]; then
    echo "client exited with status ${CLIENT1_STATUS} / ${CLIENT2_STATUS}" >&2
    SMOKE_STATUS=1
fi

if [ "${SMOKE_STATUS}" -eq 0 ]; then
    echo "ipc-fabric smoke test passed"
fi

exit "${SMOKE_STATUS}"
