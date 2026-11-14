#!/usr/bin/env bash
# -----------------------------------------------------------------------
# Demo test for the HTTP streaming PoC.
#
# Starts the mock server, waits until it accepts connections, then runs
# the demo across the three scenarios (stream / stall / flaky) with both
# transports. The demo itself asserts the per-scenario acceptance
# criteria and exits non-zero on failure, so this script only needs to
# orchestrate the server lifecycle and aggregate results.
# -----------------------------------------------------------------------
set -u

SERVER="$1"
DEMO="$2"
PORT="${COG_POC_TEST_PORT:-18765}"

log() { echo "[demo-test] $*"; }

"$SERVER" --port "$PORT" >"server.log" 2>&1 &
SPID=$!

cleanup() {
    kill "$SPID" 2>/dev/null
    wait "$SPID" 2>/dev/null
}
trap cleanup EXIT

# Wait for the server to accept connections (bash /dev/tcp probe).
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
    log "server did not become ready; see server.log"
    exit 1
fi

fail=0
for sc in stream stall flaky; do
    if "$DEMO" --client both --scenario "$sc" --port "$PORT"; then
        log "scenario $sc: PASS"
    else
        log "scenario $sc: FAIL"
        fail=1
    fi
done

exit "$fail"
