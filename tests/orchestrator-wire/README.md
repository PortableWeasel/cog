# Pure orchestrator codec checks

Ticket `9ccb5d5`, approved N and O3 layout `fa9fc05`, implementation base O
integration `d17c924`. This lane owns only `orchestrator_wire.c` and these focused
fixtures. Runtime `orchestrator.c`, shared CMake and native lifecycle fixtures
belong to the runtime author; no implementation/header fallback or stubs exist.

The production source implements all four declared CGW1/CGH1 codecs using
bounded scalar/UTF-8 helpers, explicit big-endian byte access and transactional
validation before output capacity/writes. Signed wire outcomes decode without
implementation-defined unsigned-to-signed conversion. The only borrowed ID on
success points into the caller's explicitly supplied output storage. No native,
store, application-role or execution authority follows from a successful codec.

Run from any directory:

```
python3 /path/to/checkout/tests/orchestrator-wire/check.py
```

This compiles the actual production source directly with its public declarations,
then runs strict GCC, strict Clang and GCC ASan/UBSan/LSan fixtures sequentially.
No runtime replacement or unimplemented module is linked. Build products, logs
and TMPDIR are in this checkout's `build-wire`; each command is single-process
compilation, independent of the whole-project heavy build slot. The warning
flags match the project strict set with Werror and GNU's additional logical-op.

Meaningful assertions include:

- Independent golden CGW37 and CGH63 frames fixing byte order and signed outcome.
- Every worker/handoff kind; all19 known ACK outcomes; both local-peer directions;
  maximum child count, epochs, peer/instance/checkpoint values and127-byte ID.
- Every truncated byte length of valid golden frames, extra trailing bytes,
  unsupported recognizable versions/kinds, required pointer/query behavior and
  every undersized encoder capacity. Decoder ID capacity is tested separately.
- Direct malformed wire tuples, zero/equal peers, exhausted/skipped epochs and
  wrong phase checkpoint values, without relying on encode validation.
- UTF-8 scalar boundary values, surrogate/overlong/out-of-range/truncated forms,
  embedded NUL, empty and128-byte IDs; full validation precedes capacity errors.
- All256 values at each byte position of both golden frames: every successful
  decode re-encodes to exactly the mutated bytes; every error preserves output
  metadata/storage/required. Invalid encodes likewise preserve every output.
- Always-active CHECK oracles (not disabled by NDEBUG). Wrapped allocation/free
  symbols abort, proving no codec-owned allocation or cleanup on exercised paths.

`registration.cmake.txt` is an additive handoff for the runtime/integration owner.
It is deliberately not installed or included by this lane. Actual integration
must register the real source in cog-orchestrator and link the test to that
library after independent codec acceptance.

Fresh writer evidence: all three profiles passed; logs
`build-wire/{gcc,clang,asan}-{build,test}.log`. `git diff --check` passed. These
are scoped codec claims only, not native lifecycle, O3 runtime, scenario, O or P
acceptance. Independent review remains required on the final committed source.
