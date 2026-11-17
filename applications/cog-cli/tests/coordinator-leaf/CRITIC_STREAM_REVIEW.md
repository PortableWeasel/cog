# Preliminary coordinator source review

Reviewed host_runtime.c at aaf2933 (source unchanged from 5aaa9ec).
Read full translation unit against private composition/control contracts.
This is scoped FAIL, not full B/O4 acceptance.

Confirmed defects:
- 0bc5868: retained command lookup omits COMMAND/CANCEL lane. Independent
  COMMAND wire3 and CANCEL wire3 collide and return PROTOCOL (-15).
- c1f4dde: expired physical client cell retains prior logical lease watermarks.
  Actual expiry, new HELLO lease8, exact HELLO ACK, then RESYNC wire3 returns
  INVALID_STATE (-9) against old lease7 watermark100.

`test_stream_identity_critic.c` includes the exact production coordinator source
and links actual projection/control/library implementations. It intentionally
exits1 until both bugs are fixed. The distinct wire4 CANCEL control successfully
creates a DENIED final receipt, showing the input/authority path itself is valid.
No stub implementations or source changes in the author tree.

Independent strict C11 GCC and Clang compile PASS; regression reproduces both
failures on both compilers. Existing admission, group, and receipt-pressure tests
PASS 3/3 under each compiler. Admission/group compile exact reviewed host_runtime.c
explicitly; receipt/regression include it directly. Section GC isolates real leaf
paths. Dependencies are stable copied actual A archives from close-fix f504c01
snapshot (B65e73db/C7b0fcc8/D7d1afe0 and client07d74a4); immutable inventory in
review-worker-delegation/build-worker-review/archive-sha256.txt. No sanitizer,
native/TLS, full application or whole-lane acceptance claim.

Reviewer build outputs and commands: review-host-runtime/build-host-review/.
TMPDIR was its tmp directory. No heavy build used.

Cross-lane issue 8dab83f separately tracks old-executor serialN versus freshly
admitted executor serial1 during repeated handoff. Required oracle preserves
same-stream wire watermarks and HANDOFF disposal receipts while resetting only
per-executor VIEW/POLICY metadata after factual retirement/new admission. D author
cannot independently approve that fix. No executable repeated-native oracle yet.

Retained driver outcome versus close cleanup outcome was checked and is not a
finding: close(primary=OK) must truthfully report cleanup, per amended contract.
Failed reload purpose4 replay remains separately tracked design work, not waived.
