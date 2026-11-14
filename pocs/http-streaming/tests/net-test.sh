#!/usr/bin/env bash
# -----------------------------------------------------------------------
# Wrapper for the network error-path tests.
#
# Starts the mock server (needed for the 404 case), exports its port to
# the test binary via COG_POC_MOCK_PORT, and runs the test.
# -----------------------------------------------------------------------
set -u

SERVER="$1"
TEST="$2"
PORT="${COG_POC_TEST_PORT:-18769}"

"$SERVER" --port "$PORT" >"server.log" 2>&1 &
SPID=$!

cleanup() {
    kill "$SPID" 2>/dev/null
    wait "$SPID" 2>/dev/null
}
trap cleanup EXIT

ready=0
i=0
while [ "$i" -lt 100 ]; do
    if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
        exec 3>&- 3<&- 2>/dev/null || true
        ready=1
        break
    fi
    sleep 0.1
    i=$((i + 1))
done
if [ "$ready" -ne 1 ]; then
    echo "[net-test] server did not become ready; see server.log" >&2
    exit 1
fi

COG_POC_MOCK_PORT="$PORT" "$TEST"
exit $?
