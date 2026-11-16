# Guard release ownership on failure

Design ticket d97a81b, reproduced defect fae1e27. Header-only clarification;
no function signature, struct layout, allocation, opcode or vtable slot changes.

## Pass 1: one result classification for public and backend guards

For a valid live guard, INVALID_STATE is the only nonconsuming refusal. Active
public borrowers are checked before the backend callback. A backend returning
INVALID_STATE must likewise leave its admitted handle/resources intact. After
borrow return or correction of that refusal, another release may be attempted.

OK and cleanup errors consume the exact backend handle and public wrapper once.
In particular, native close IO must not leave a dangling public alias or make
callers retry a numeric descriptor which may already have been released/reused.
The backend completes best-effort teardown and reports its actual error; the
wrapper unlinks/frees its bookkeeping under the same classification. A NULL
public guard is an OK no-op. Invalid/dangling foreign pointers are outside the
live-handle precondition, not additional ownership-return cases.

The error remains factual. Consuming a handle does not prove successful lock
release, target preparation, CAS authority or handoff completion. A frozen source
clears the consumed alias, retains failure and never reports successful release
or resumes the old epoch. Checked destruction proceeds through remaining owned
resources after consuming cleanup errors, preserving the first error.

This is an explicit semantic amendment for custom fenced backends, not a claim
that old retain-on-IO callbacks already conform. They must adopt the same result
classification before use with the amended wrapper. No wrapper can infer backend
consumption from a pointer, backend identity, or a second release call. No ABI
layout change or automatic adapter hides this compatibility obligation.
