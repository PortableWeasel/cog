# Stage 14 replay primitive implementation evidence

Ticket 91108c5 implements the bounded in-memory replay owner and CGR1 codec
specified by `cog/ipc.h` and scenario 11/R13. The owner reserves metadata and
payload storage at construction. Successful append copies bytes, evicts only
the oldest records needed, and never allocates; reads and codecs do not advance
shared state. Generation mismatch and lost prefixes yield explicit GAP records.

CGR1 payload length is a 32-bit wire field. Append accepts payloads up to the
configured in-memory bound even if a payload cannot fit that wire field; encode
returns LIMIT before reading such payloads or writing output. Consumers sending
CGR1 must configure a wire-compatible payload bound. This is an explicit codec
representation boundary, not a truncation or eviction rule.

`test_ipc_replay` covers retention and byte/entry eviction, empty and stale
windows, cursor bounds and exhausted sequences, binary and zero-length payloads,
copy ownership and capacity retry, wire validation, UTF-8 and version/kind
rejection, and each constructor allocation failure. Linker allocation wrappers
verify append, copy, encode and decode perform no runtime allocation. Strict
GCC, Clang and ASan/UBSan focused runs provide local evidence. Full application
reconnect/snapshot behavior remains the separate O integration task.

## Independent critic review — cycle 1/4

Reviewed implementation `33db0d85fc8c9c2e854d9cdadde9d960d9dc4257` against
frozen `ipc.h`, R13/scenario 11, REAL_CLIENT_CONTRACTS and PASS3. Verdict: PASS
for this primitive delivery; no production or public-header amendment required.

The additional `unit-ipc-replay-critic` test checks 128 entry/byte capacity
combinations over 25,600 deterministic append steps against a separate retained
record model, including exact eviction, cursor/generation branches, repeated
nonconsuming reads and transactional capacity retries. Independent wire building
and specification predicates exercise 4,608 scalar combinations across
DATA/GAP/CURSOR. Golden big-endian bytes, every single-bit mutation of four
valid envelopes, truncations, trailing bytes, 12,000 deterministic randomized
malformed frames, strict UTF-8 boundaries, copied-view survival after eviction
and destruction, and constructor failure/free accounting extend the writer's
coverage. The writer's allocation wrappers also cover realloc and the uint32
wire representation limit without allocating a huge payload.

All five IPC suites passed in fresh GCC/Werror, Clang/Werror and Clang
ASan/UBSan builds (`build-replay-critic-gcc`, `build-replay-critic-clang`,
`build-replay-critic-asan`), with leak detection and halt-on-error enabled for
sanitizers. The public IPC header also compiled standalone under both strict
compilers. No warnings or sanitizer diagnostics occurred in these targets.
The attempted full default Werror build reproduces the unrelated existing
scenario-15 missing `cog_tool_t.async` initializer; full release validation is
not claimed. In-place overlapping codec buffers are not a promised API facility
and were not assumed by the review. O still owns headless/snapshot integration.
