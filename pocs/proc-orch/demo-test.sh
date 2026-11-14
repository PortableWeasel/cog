#!/bin/sh
# Demo test for the process-orchestration PoC.
#
# Runs the orchestrator (which spawns and monitors its own children) and
# verifies that every acceptance criterion produced the expected event:
#
#   - N=3 children are spawned and each sends heartbeats;
#   - a 'spawn subagent' request to child 1 is acknowledged;
#   - child 2's exit (SIGTERM from the orchestrator) is detected;
#   - child 3's heartbeat timeout (lazy worker) is detected.
#
# Usage: demo-test.sh <path/to/proc-orch>

set -u

ORCH="$1"
LOG="demo.log"

rm -f "${LOG}"

"${ORCH}" > "${LOG}" 2>&1
STATUS=$?
cat "${LOG}"

if [ "${STATUS}" -ne 0 ]; then
    echo "proc-orch exited with status ${STATUS}" >&2
    exit 1
fi

# --- Acceptance 1: N children spawned and heartbeating -------------------
grep -q 'spawned child=1' "${LOG}" || { echo "FAIL: child 1 not spawned" >&2; exit 1; }
for c in 1 2 3; do
    grep -q "heartbeat child=${c}" "${LOG}" || {
        echo "FAIL: no heartbeat from child ${c}" >&2; exit 1; }
done

# --- Acceptance 4: spawn-subagent request acknowledged -------------------
grep -q 'spawn-sent child=1 subagent=1001' "${LOG}" || {
    echo "FAIL: spawn request not sent to child 1" >&2; exit 1; }
grep -q 'ack child=1 subagent=1001' "${LOG}" || {
    echo "FAIL: child 1 did not acknowledge spawn request" >&2; exit 1; }

# --- Acceptance 3a: child exit detection -----------------------------------
grep -q 'exit child=2' "${LOG}" || {
    echo "FAIL: child 2 exit not detected" >&2; exit 1; }

# --- Acceptance 3b: heartbeat timeout detection ----------------------------
grep -q 'timeout child=3' "${LOG}" || {
    echo "FAIL: child 3 heartbeat timeout not detected" >&2; exit 1; }

# --- Clean shutdown --------------------------------------------------------
grep -q 'orchestrator done' "${LOG}" || {
    echo "FAIL: orchestrator did not finish cleanly" >&2; exit 1; }

echo "proc-orch demo test passed"
exit 0
