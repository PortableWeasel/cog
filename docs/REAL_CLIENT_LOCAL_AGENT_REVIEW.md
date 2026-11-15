# Core-local agent lifecycle independent review

Review ticket `4cde520`, package `bf7a450`, revision 1/4: **PASS**.
Reviewed implementation checkpoint `1b4374c` on
`codex/test-agent-migration`, including `224cf58`, `e770bac`, the header-first
amendment `f531420`, and narrow design ticket `86f7a3c`. This review adds only
regressions to the existing agent test and this evidence; no production source,
public contract, CMake registration, or application scenario behavior is changed.

## Contract and ownership review

Three targeted passes independently retraced the amendment:

1. Header surface: `agent.h`, `event.h`, and `core.h` agree on copied resolved
   configuration, the local default epoch, typed intent identity, explicit
   caller capacity, and lease lifetime. The two intent kinds are appended after
   the previous final event kind; earlier values are unchanged. Generated
   dispatch IDs equal the intent kind, while unrelated application events may
   retain the same numeric ID with another kind. Static dispatch remains the
   application execution boundary.
2. Scenarios 03/09/10: local and remote admissions expose the actual attempt
   epoch before updates; handlers copy task/optional parent/scalars and check
   cancellation before starting effects. Remote authentication, forwarding,
   placement, drain/reap, and owned notification event storage remain explicit
   application/orchestrator work. No implicit execution or wire send was added.
3. Consistency and failure paths: queue-full spawn publishes no record, child,
   output ID, or lease; queue-full first cancel leaves cancellation unadmitted.
   Duplicate admitted cancel adds no event. Intent storage survives queue reuse,
   updates and dispatch. Release refuses terminal-but-unacknowledged records
   and queued/in-handler leases. Dispatch releases the popped lease after every
   result; later queued intents remain protected. Copy outputs remain independent
   after release/destruction. Retained update correlation, duplicate comparison,
   sequence order, backpressure, ack and terminal immutability match the public
   API. Legacy private transition hooks delegate to public update admission and
   never auto-ack.

The bounded registry allocates at `cog_core_new`; spawn's explicit child-session
constructor is its documented allocation boundary. Runtime update/copy/codec/
ack/result/cancel/dispatch/release add no hidden allocation or mutable production
global state. CGA1 uses explicit big-endian widths, rejects malformed UTF-8,
unknown values and nonexact frames, and preserves outputs on failure. Public
result reads retain failure-terminal output and permit capacity retry.

## Independent execution evidence

Fresh review builds used GCC 14.2.0 and Clang 19.1.7, C11, Debug, the repository's
strict warning set, and `COG_ENABLE_WERROR=ON`.

| Check | Result |
|---|---|
| GCC and Clang: session, session-codec, session-allocations, event, state, core, agent, core-service | 8/8 passed per compiler |
| GCC ASan+UBSan: core, agent, core-service; leak detection and halt-on-error | 3/3 passed |
| GCC TSan: core, agent, core-service; halt-on-error | 3/3 passed |
| Allocator-wrapped agent test (`malloc`, `calloc`, `realloc`) | Passed, zero calls during tracked runtime operations, including copied config and CGA1 encode/decode |
| Public standalone headers under GCC/Clang C11 and G++/Clang++ C++17 | 26 headers per compiler, 104 successful objects |
| Original scenarios 03/07/09/10 with strict GCC and Clang | All four targets compiled under both compilers |
| `git diff --check` | Passed |

Additional regressions use the real public enqueue/admission/run path. They
cover cancellation admitted before spawn dispatch with zero execution effects;
immutable queued intent through queue-slot reuse; independent config after input
mutation, release and core destruction; unrelated application use of the same
numeric dispatch ID; release after a NULL-handler UNHANDLED exit and a denied
transition; malformed status/outcome/length/NUL/trailing/truncated codec frames
with unchanged metadata, storage and lengths; invalid UTF-8 encoder output
atomicity; and successful overlapping in-place decode. Existing regressions
cover handler error, missing dispatch row, full-queue admission rollback,
retention, stale peer/epoch/sequence, duplicates, failure results and slot reuse.
No fake intent events, runtime allocation stubs, real network or credentials
were used.

Reproduce focused runtime checks with the repository CMake commands and
`ctest --test-dir <review-build> -R '^unit-(agent|core|core-service)$' --output-on-failure`.
Review build directories are `build-critic-agent-gcc`, `build-critic-agent-clang`,
`build-critic-agent-asan`, and `build-critic-agent-tsan`.

## Scope limits

A full default strict build remains blocked by existing application fixture drift:
`applications/cog-cli/tests/test_15_multi_turn_tools.c:86` lacks the appended
`cog_tool_t.async` initializer under both GCC and Clang. That file is unchanged
by this package and belongs to the pending application/tool migration work.
Consequently this is scoped core-local acceptance, not a claim that the full
CTest or release gate is complete. Original scenario 03/07/09/10 runtime
integration, K conversation-loop migration, N tool integration and O remote
orchestrator/application delivery remain pending. No merge is performed by
this review.
