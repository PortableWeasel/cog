#!/bin/sh
# Edge-case test for the IPC fabric PoC.
#
# Usage: edge-test.sh <path/to/ipc-server> <path/to/ipc-edge-test>

set -u

SERVER="$1"
TEST="$2"
SOCKET="${COG_IPC_SOCKET:-/tmp/cog-ipc-poc-edge.sock}"

export COG_IPC_SOCKET="${SOCKET}"
rm -f "${SOCKET}"

"${SERVER}" > server.log 2>&1 &
SERVER_PID=$!

cleanup() {
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
    rm -f "${SOCKET}"
}
trap cleanup EXIT

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

"${TEST}" > edge.log 2>&1
STATUS=$?

cat edge.log || true
exit "${STATUS}"
