# Stage 14.I toolkit writer evidence

Delivery `03f60d7`, implementation children `c3b93b8` / `dd36fcf` / `d3b4f0b`,
independent review `036ac33`, integration `8160196`, finalize `474323d`.
Writer branch `codex/impl-toolkit`, based on integrated F/G/Q main `e35dc9f`.
Independent review and real-client integration remain pending. This is writer
validation, not acceptance gate M or a full release claim.

## Implemented boundary

The approved public core/tool and toolkit/workspace headers are unchanged.
Implementation covers copied synchronous calls, fixed at-most-once capture,
pre-effect batch reservations/startup barriers, strict bounded schema/policy
admission, guard borrowing and start-time unsafe-session checks, retained async
results, descriptor-relative workspace read/search/edit, explicit command
permissions/environment, helper-local process supervision and complete supported
descendant reap. The [module contract](../libraries/cog-toolkit/contracts/CONTRACT.md)
records defaults, limits, result formats and platform/cancellation boundaries.

Cog-owned runtime paths do not allocate. `workspace_new` reserves file/edit/
traversal scratch; the first file execution borrows it, and additional prepared
executions reserve their own scratch at their explicit constructor. Parser
allocations remain in bounded explicit pools with no allocator fallback.
Allocation-fault tests iterate constructor failure points and reject all wrapped
malloc/calloc/realloc during runtime read/copy/destruction. No production doubles
or success stubs are used: every registered test links real core/session/store.

The process helper alone sets subreaper/session/signal state and owns the shell
process group. The host does not change cwd or child/signal handling. Commands
receive explicit copied environment and isolated stdio. Capture truncation keeps
draining. TERM-resistant descendants require the configured KILL deadline;
ordinary grandchildren surviving shell exit remain owned/adopted/reaped before
a terminal result. Commands grant process execution, not OS confinement. Group
escape/deliberate daemonization is outside the supported cancellation profile.

## Tests run

Writer validation on 2026-11-15:

| Build | Registered tests | Result |
|---|---|---|
| Strict GCC C11/Werror | toolkit, critic, parallel, async, workspace, resources, fencing, review-faults | 8/8 |
| ASan+UBSan, leak detection, vendored codec instrumented | same | 8/8 |
| TSan | critic concurrency/barrier and bounded worker/failure suite | 2/2 |

The dedicated tests cover:

- Complete strict JSON, Unicode/scalar lengths, decoded duplicate keys, integer
  exactness/ranges, nested schemas/enum/items, unsupported keywords and limits.
- UNDECIDED/DENY with zero effects, prepared cancel, repeated start refusal,
  delivery overflow preserving destination and one retained invocation.
- Constructor OOM without published handles; fixed capture overflow without
  replay; batch setup and partial pthread creation failure before effects.
- Missing/mismatched real owner guards, live borrow preventing guard release,
  synchronous fenced-store refusal, and late INDETERMINATE preventing admission
  under an otherwise live guard.
- Disposable read/search/atomic edit, zero-effect denied edits/commands,
  traversal/absolute/NUL/symlink refusals, limits, copied environment and explicit
  process exit/truncation/effect metadata.
- Timeout, repeated cancellation, shell-exited grandchildren and TERM-resistant
  KILL cleanup; grandchild PIDs are absent and no waitable children remain.

Reproduction (toolkit targets avoid unrelated unmigrated application/provider
fixtures):

```sh
cmake -S . -B build-toolkit -DCOG_ENABLE_WERROR=ON -DCOG_BUILD_CLI=OFF
cmake --build build-toolkit --target test_toolkit test_toolkit_critic \
  test_toolkit_parallel test_toolkit_async test_workspace \
  test_toolkit_resources test_toolkit_fencing test_toolkit_review_faults -j4
ctest --test-dir build-toolkit -R 'unit-(toolkit|workspace)' --output-on-failure

# Repeat in a separate directory with -DCOG_ENABLE_ASAN=ON.
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build-toolkit-asan -R 'unit-(toolkit|workspace)' --output-on-failure

# Separate TSan build, with -DCOG_ENABLE_TSAN=ON.
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-toolkit-tsan \
  -R '^unit-toolkit-(critic|parallel)$' --output-on-failure
```

## Dependency and packaging evidence

Production vendors unmodified yyjson 0.12.0 under the toolkit, separate from
research `pocs/`; SHA256/version/license are in `vendor/yyjson/SOURCES.md`.
`generate_prefix.py --check` verifies the checked-in prefix map. `nm -g
--defined-only` over the toolkit archive found no unprefixed `yyjson_` or
`unsafe_yyjson_` exports. The object is absorbed into the static archive; no
consumer yyjson include/link ABI is exposed. The MIT license installs under
`share/licenses/cog-toolkit/`.

A temporary install of the actual result/core/IPC/obs/toolkit targets was consumed
by an out-of-tree CMake application. It imported those installed target files,
linked only `cog::toolkit`, created/freed a copied call, and referenced all four
workspace descriptors successfully. The toolkit export contained no source,
worktree, build or yyjson target dependency. This proves the toolkit target's
installed dependency closure with the migrated primitive libraries, not a full
project install or release relocation gate P.

No network, provider, paid model, persistent user workspace or user commands were
used by these checks. Workspace/process fixture effects stayed under their
own `mkdtemp` roots and were removed on successful completion.

## Independent review revision 1

Review `036ac33` found that the initial nonblocking TERM send could be lost after
EINTR/EAGAIN: the execution was already cancelling, and later polls attempted
only KILL. Critic checkpoint `6fcafcc` added the reproducer and an edit fsync
failure oracle. Before the fix it recorded one failed TERM attempt, no cleanup
marker and exit 137.

The revised lifecycle distinguishes cancellation requested, TERM accepted and
KILL accepted. Cancel queues pending control work. Each poll attempts at most
one nonblocking send, retaining EINTR/EAGAIN for later service. Repeated cancel
and send retries never extend the original escalation deadline. Unsent TERM is
superseded by KILL at that deadline; transient KILL failure is retained too.

The permanent `test_toolkit_review_faults` now covers single/multiple EINTR and
EAGAIN followed by successful TERM handling, both manual cancellation and tool
timeout, persistent TERM backpressure with transient KILL failures, and zero-grace
KILL. It checks one send attempt per poll, one accepted control command, retained
ABORTED/TIMEOUT outcomes, one command start, repeated result copies, absence of
both shell/descendant PIDs and no waitable children. The critic's original edit
fsync case still proves unchanged target and removal of the owned temp.

Revision writer validation is strict 8/8, ASan+UBSan/leaks 8/8 and TSan concurrency
2/2. Successful retry cases observed TERM accepted on attempt 2 or 4, marker
present and exit 0; persistent-backpressure cases observed no accepted TERM,
KILL accepted on attempt 3 and exit 137. Independent rereview remains pending.
