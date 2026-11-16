# O1 independent source/runtime review

Candidate: writer evidence `3cc0219abe0a223720ffd2b086d385b0880a7e75`, production
source `fa921ac`, approved O base `0ae8213`. Separate managed review worktree
`review-headless-ipc`, branch `codex/review-headless-ipc`. No production/public
header/core/replay/app/orchestrator edits. This receipt covers IPC only; the O2
saturated-core-event/ingress proof capture join remains an explicit future oracle.

Reviewed the complete `ipc.c`, approved `ipc_internal.h`, public auth/directed
send/wipe contracts, private one-unit service API, layout's first-proof barrier,
new authored fixture and additive unit CMake. Kernel SO_PEERCRED is attempted once
on actual native establishment; query is read-only and fails closed without
invented HELLO metadata. Once readiness publishes, the private service unit
returns before payload/EOF/TX work. Public aggregate service may cross that
boundary and is explicitly not the core proof-capture mechanism.

Constructor checked products cover descriptor and payload reservations. Partial
constructor unwind wipes each allocated payload pointer independently. No
runtime growth was added. Directed send checks one actual live list entry and
uses its copied TX admission; it neither broadcasts nor mutates routes. Physical
failure clears abandoned TX/incomplete RX while leaving committed RX readable;
receive OVERFLOW retains that exact slot, logical recv retirement wipes the whole
slot, explicit free/disconnect wipes every allocated payload byte. Opt-out and
separately-owned accepted-endpoint teardown semantics remain unchanged.

Independent fresh CMake strict Werror builds on the frozen admitted dependency
profile (no installs, byte changes or LD overrides):

| Profile | Exact scoped result |
|---|---|
| GCC Release/O3 |6/6 unit-ipc, ipc-critic, ipc-nonblocking, ipc-replay, ipc-replay-critic, ipc-authenticated |
| Clang Release/O3 | same6/6 |
| Clang Release/O3 ASan+UBSan+LSan | same6/6; leak detection and halt-on-error |
| GCC Debug TSan |4/4 unit-ipc, ipc-critic, ipc-nonblocking, ipc-authenticated; halt-on-error |

Authored O3 fixture compilation includes -UNDEBUG and allocator builtin controls;
positive allocation counts, before-free canaries, real child PID and payload
checks execute. Tests use real Unix sockets/fork rather than synthetic credential
claims. No full O build/scenario/installed acceptance is inferred from these
component targets.

The independent `test_ipc_o1_review.c` adds three joins:

1. Before FIRST accepted-endpoint service, raw peer queues two complete frames,
   a partial next frame and FIN. The one handshake service publishes identity
   with zero recv calls and zero retained frames. Peer info then copies real PID
   with no additional getsockopt. After accepting/retiring the first frame, the
   RX head is1; partial next data wraps into slot0. Actual EOF clears slot0 but
   retains complete slot1, including across a small-buffer OVERFLOW. Its eventual
   successful receive wipes that whole slot. Cached proof survives as an owned
   copy; public post-EOF query correctly returns NOT_FOUND unchanged.
2. Saturated peer A returns AGAIN while peer B still copies a frame. Explicit
   disconnect/wiping A cannot discard B. B's exact identity+length+payload bytes
   arrive on its actual socket, then its retired slot is wiped. Never-used
   malloc capacity is not falsely required to be zero before explicit teardown.
3. Runtime auth/query/service/send/recv paths invoke zero allocator wrappers;
   constructor positive control invokes exactly6. SIZE_MAX frame/count/route
   products reject with NULL/INVALID_ARG before any allocation.

All three joins passed GCC/Clang O3/NDEBUG, Clang O3 ASan+UBSan+LSan and GCC O3
TSan via `tests/unit/run_ipc_o1_review.py`. Each also rejects its deliberate
CHECK(false) control. No removable assert is used in the independent fixture.
The raw receive uses actual readiness with a bounded poll, not a short sleep.

Reproduce the independent probe:

```sh
python3 tests/unit/run_ipc_o1_review.py --cc gcc --build-dir /tmp/cog-o1-review-extra-gcc
python3 tests/unit/run_ipc_o1_review.py --cc clang --build-dir /tmp/cog-o1-review-extra-clang
python3 tests/unit/run_ipc_o1_review.py --cc clang --sanitize address --build-dir /tmp/cog-o1-review-extra-asan
python3 tests/unit/run_ipc_o1_review.py --cc gcc --sanitize thread --build-dir /tmp/cog-o1-review-extra-tsan
```

Integration owner may register the independent C fixture linked to cog::ipc with
--wrap=getsockopt,recv,malloc,calloc,realloc,free; use project strict warnings,
allocator builtin controls, and timeout20. Shared CMake remains untouched by this
reviewer. Its standalone runner already builds actual ipc.c, no replacement body.

Fresh suite logs and SHA256:

| File | SHA256 |
|---|---|
| /tmp/cog-o1-review-gcc.log | bad867496a78be5eb21398505a2376b5ecf34b1209347f9e6359939efea0bb7c |
| /tmp/cog-o1-review-clang.log | e439f30c335428354ccd9a9140e5abdc2f52f060c0c16f97b5c71f2b97a9861f |
| /tmp/cog-o1-review-asan.log | 4a284ffcb9330c74dbc4f9a002f82913f7f575f866b5193f882d27dbc9f367e2 |
| /tmp/cog-o1-review-tsan.log | e11f525959cf7866eb499975fee9f76b74e90aad8d3366da18647f3646c794ca |

No paid requests or dependency/profile/build_user changes. Root owns O-only
integration/finalization, and O2 owns actual core proof-queue saturation coverage.

Architect clarified identity scope during final review: a listener lifetime is
one active listen→disconnect fabric incarnation, not endpoint allocation→free.
A later listen creates a new fabric; callers must retire old bindings. N's host
creates fresh runtime/fabric identities and does not carry stale attempt/peer
bindings across that boundary. Existing listen resetting its per-fabric counter
therefore does not weaken the claimed monotonic/no-alias guarantee within one
active listener. No stronger endpoint-allocation-wide uniqueness is claimed.

**Verdict: independent O1 SOURCE/RUNTIME PASS at the exact candidate above.**
No concrete source defect or missing O1 contract operation found. Independent
fixture/evidence additions are test-only; keep them alongside the writer history.
This does not approve O2/O3/app composition, the whole O gate or a main merge.
