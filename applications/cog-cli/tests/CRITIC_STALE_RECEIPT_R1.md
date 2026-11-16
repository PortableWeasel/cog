# Scoped stale-receipt R1 review

Independent source review cd5b7b4c29772c02b8a1b16ebca821f75c3c19c5,
production delta 36ccab4: PASS b99c6eb only.

Typed POLICY and VIEW receipt validation precedes covered reply_to retirement;
covered responses return AGAIN without touching pending/new executor state.
Live mismatches retain errors. COMMAND request retirement and our response-ID
retirement use their respective inbound/outbound counters. UPDATE validates the
inner/outer tuple before treating a covered request as stale. A missing retired
child cannot mutate another physical occupant; the live same-agent conflicting
identity check runs before high-watermark retirement. Actual native worker/peer
and runtime validation still precedes these classifications.

Actual production-source fixtures independently PASS 4/4 strict GCC, 4/4 strict
Clang and 4/4 Clang ASan/UBSan/LSan for owned source. Real IPC duplicate POLICY,
VIEW after actual disposal, command ACK and retired child receipts are exercised.
Reviewer added negatives for malformed covered policy state, zero VIEW serial,
retired UPDATE inner/outer attempt mismatch, and foreign managed worker. All
remain rejected without changing the physical occupant. No implementation edit.

Dependencies are immutable actual archived libraries from
review-worker-delegation/build-worker-review/joined with adjacent SHA inventory.
They remain uninstrumented: sanitizer coverage is owned worker/delegation source
and fixture code only, not full dependencies. Runner uses strict C11 Wall Wextra
Wpedantic Wconversion Werror, function/data sections and section GC. Logs and
TMPDIR are checkout-local build-worker-leaf. No heavy build or stubs.

Only b99c6eb is closed. Full C/O4 requires joined native two-round, cancellation
and handoff evidence. This does not review or approve D handoff source originally
authored by this reviewer, nor close its serial-reset ticket.
