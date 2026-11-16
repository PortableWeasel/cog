# Worker and delegation branch fixtures

`test_worker_receipt_owned.c` includes the actual worker source to exercise its
private bounded receipt and result handlers; no replacement implementation is
linked. A real IPC listener/client/accepted endpoint with one TX descriptor
forces confirmation AGAIN. Exact bytes/correlation remain stable; successful
copied send survives wiping the local arena. Receipt replay, conflicting bytes,
terminal-fixed versus provisional/cancel dispositions, cleanup identity limit,
full output escaping, truncated/uncertain/malformed result and mailbox limits
have explicit checks.

`test_delegation_callbacks.c` calls actual callback bodies for per-handle clock
and state boundaries: equal/backward timestamps, independent handles, unchanged
outputs/state on invalid pointers/samples, and expired start retains its checked
sample. Constructor argument parser checks include UTF-8/escaped text and invalid
NUL/duplicates/empty batches. These do not claim complete executor construction,
provider execution, or whole worker native-loop coverage.

Both fixtures use production source and real linked libraries. For a small direct
run without rebuilding dependencies:

```
python3 applications/cog-cli/tests/run_worker_leaf.py /path/to/actual/joined-build
```

The runner uses strict GCC and Clang and checkout-local `build-worker-leaf` logs,
executables and TMPDIR. The supplied archives must be from the coherent O4 join.
`worker_leaf.cmake.snippet` is for the shared CMake owner's registration, including
full sanitizer profiles later. The direct GCC/Clang run does not claim sanitizer
coverage of prebuilt libraries. Native worker/bootstrap/delegation integration
uses the separate real loopback TLS oracle; no live provider calls are needed.

`test_worker_handoff_critic.c` is the independent cycle-1 regression, extended
with actual profile-only configuration cleanup and wrong-checkpoint rejection.
It covers exact purpose3 replay before admission checks and retention of prior
consuming cleanup failure across a fresh ABORT.

`COG_LEAF_SANITIZE=1` runs the three source-including fixtures with Clang
ASan/UBSan; this instruments the owned worker/delegation bodies and test code.
The supplied prebuilt archives remain as built: this optional direct run is not
a claim of fully instrumented dependency or whole-runtime validation.
