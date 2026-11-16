# Handoff leaf checks

`test_handoff_leaf.c` includes the actual leaf for its private helpers and links
real configuration, executor, delegation, core, provider, toolkit, orchestrator,
IPC and durable store implementations. It defines no replacement API bodies.

Coverage:
- Exact immutable request retry; reply-to, managed role, owner and checkpoint
  rejection before state changes; source versus target PREPARED distinction.
- Full original/final retention, same-stream retirement, PROFILE independence,
  unrelated-key refusal, native-terminal failure without disposal/reuse proof.
- Actual idle guarded source checkpoint and exact duplicate; actual STAGED
  construction; CAS blocked while source guard remains; pre-CAS target guard
  refusal; actual release, CAS, guarded target, route and activation; no startup
  provider send; real bounded source/target drain and checked close.

The local operations are component tests, not an admitted local-to-local move
(which the public request correctly forbids). Full local/remote and remote/remote
native-worker joins, lost transport receipt retries and failure injection remain
required in the coherent application review.

Additive registration for the app owner (do not also compile handoff.c directly
into this executable; it is included for leaf inspection):

```cmake
add_executable(test_handoff_leaf test_handoff_leaf.c)
target_include_directories(test_handoff_leaf PRIVATE ../src)
target_link_libraries(test_handoff_leaf PRIVATE cog::cli)
target_enable_warnings(test_handoff_leaf)
# Harness supplies fresh checkout-local durable directory and socket path:
# test_handoff_leaf <fresh-store-directory> <socket-path>
```

Independent writer evidence in `impl-headless-handoff/build-handoff-leaf`:
GCC and Clang strict C11 compile/link/run passed. ASan/UBSan/LSan direct leaf and
bridge compilation/run passed, linking previously built **uninstrumented** real
A/library archives; this is not a full sanitizer application profile. All inputs
were copied before A rebuilt: A archives at cf40868, B host source 0a8a05c, C worker
and delegation sources cca5231, approved clone declaration from 8c4d0e5. Those
snapshots and exact compiler driver remain in the ignored evidence directory.
No moving source or archive was used during the checks. No shared CMake edits.
