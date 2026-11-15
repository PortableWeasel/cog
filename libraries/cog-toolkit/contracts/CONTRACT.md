# cog-toolkit contract

The approved public headers in `include/cog/` and core's `cog/tool.h` define
this implementation. Stage 14.I implements them without public API amendments.
All runtime entry points use the owner thread, except explicitly requested
legacy worker execution. No runtime Cog allocation or buffer growth occurs.

## Synchronous compatibility

`cog_toolkit_call_new` owns one block containing copied name/request, private
metadata and a 65,536-byte capture including NUL. One context means one invocation.
`execute` runs the handler at most once and caches status, output and effect
uncertainty. Delivery overflow reports the required length, preserves destination
bytes and permits only another copy. Handler capture overflow is terminal,
explicitly truncated and possibly effect-bearing; it never retries the handler.
Cancellation can be observed within a running legacy callback; cancellation
following a terminal result cannot change that result. Legacy callbacks can block.

`execute_all` defaults to sequential. Concurrency requires explicit
`max_parallel > 1`, REENTRANT|INDEPENDENT on every selected descriptor, and distinct
call contexts. All collection strings/statuses and worker slots are reserved
before dispatch. A condition-variable startup barrier ensures partial thread
creation failure produces zero invocations. `stop_on_error` stops later claims;
all launched results remain in the returned collection, with ABORTED in unstarted
slots. Worker joins finish before returning. This compatibility path refuses
`require_owner` stores with UNSUPPORTED.

## Strict codec and schema

The private unmodified yyjson 0.12.0 dependency has an explicit constructor pool,
with no default allocator fallback. Every external symbol, including unsafe
helpers, uses the `cog_toolkit_` prefix. Vendored source checksums and MIT license
are in `vendor/yyjson/`. Source and public dependency exports do not link `pocs/`.

An async constructor requires a complete strict JSON object. It rejects duplicate
keys at every level, invalid UTF-8, unpaired surrogates, malformed numbers, trailing
bytes and configured argument/schema/depth/pool exhaustion before invoking a tool
constructor. Default input/schema capacities are 65,536 bytes including NUL,
depth is 64 and each codec pool is 1 MiB. The recursive validation implementation
supports configured depth up to 256; larger reservations return LIMIT. All JSON
and schema validation happens before tool effects.

Supported keywords are `type`, `properties`, `required`,
`additionalProperties:false`, `items`, `enum`, `minimum`, `maximum`,
`minLength`, `maxLength`, `minItems`, `maxItems`; `title`, `description`, `$schema`
are annotations. Unknown validation keywords or unsupported schema forms fail
UNSUPPORTED, including in unused property schemas. String lengths count Unicode
scalars. Integers use exact decimal arithmetic in signed 64-bit range; fractions
fail integer validation. Nonfinite/unrepresentable numeric ranges fail
UNSUPPORTED. Number values use the pinned locale-independent finite double
reader. Schema numeric boundaries beyond +/- (2^53-1) fail UNSUPPORTED.

The legacy output validator uses the same strict subset with automatic local
1 MiB pool, input/schema ceilings of 65,536 and depth 64. It performs no hidden
heap allocation. NULL/empty schema means no validation.

## Async admission and lifetime

`execution_new` validates all six static callbacks before any callback, resolves
and validates the call, copies identity, and invokes only `new_`. Tool `new_`
reserves copied inputs and bounded state/output without external effects. A
missing or mismatched required owner guard fails before preparation. The wrapper
borrows a matching live guard until prepared/terminal execution destruction.

UNDECIDED returns AGAIN, DENY creates a terminal no-effect DENIED outcome, and
explicit ALLOW admits exactly one start. The application must first durably save
the corresponding STARTED ledger and apply its policy. This is an application
precondition; toolkit does not manufacture intent or permission. ALLOW rechecks
current session inspection/INDETERMINATE state and guard validity. A session
becoming unsafe after construction cannot start new effects.

`poll` and `cancel` never sleep. Backwards clocks fail INVALID_ARG. Prepared
cancellation yields terminal ABORTED. Active cancellation requires polling until
terminal cleanup. Freeing active/cancelling handles returns INVALID_STATE and
keeps them intact. Terminal result copying is repeatable; destination OVERFLOW
changes only required length/output_len. Status, truncation, effects and exit
availability are explicit and remain retained until free.

## Workspace tools

`workspace_new` opens the root with component-by-component directory/no-follow
traversal, copies its explicit environment and reserves file/edit/traversal
scratch. An execution borrows this scratch when available; additional concurrently
prepared file executions reserve their own bounded scratch at their constructor.
Root, handles, descriptors and every additional block have matching destructors.
Workspace free refuses any live execution borrow.

Model paths are relative, bounded to 4095 bytes, and reject absolute paths, `..`,
empty components, embedded NUL and symlinks in every component. All filesystem
operations use retained directory descriptors and `openat`/no-follow traversal;
the embedding process never changes cwd. Nonregular file inputs fail closed.

- `read_file(path)` reads bounded text in 4096-byte units.
- `search(pattern,path)` defaults to root; literal nonempty search emits
  `path:line:text` lines. Descriptor traversal and examined entries are bounded;
  encountered symlinks fail rather than being followed.
- `edit_file(path,old,new)` requires one nonempty literal old match. Reads and
  writes are bounded; a uniquely created same-directory temporary is fsynced and
  atomically renamed. Changed target identity/size/timestamps refuse replacement.
  Directory fsync uncertainty is INDETERMINATE. Cancel/error removes owned temps;
  cleanup failure is reported. Possible effects become true when a temp exists.
- `run_command(command,cwd)` uses `/bin/sh -c`, root-relative cwd, explicit copied
  environment and merged stdout/stderr capture. Commands default disabled; disabled
  constructors return UNSUPPORTED, and DENY never launches a shell. Absent or zero-count
  environment defaults to PATH=/usr/bin:/bin and LC_ALL=C.UTF-8. No host credentials
  or arbitrary environment entries are implicitly inherited.

Output capture stops storing at the configured ceiling but continues draining.
Truncation is explicit; a split UTF-8 tail is removed. Embedded NUL or invalid
text bytes in captured file/process output become `?` so results remain UTF-8 C
strings. File size, traversal work and output capacity are independent ceilings.
Regular-file I/O and filesystem fsync can incur OS latency: quantum bounds work
units, not hard elapsed-time guarantees.

## Process ownership

The initial process implementation requires Linux no-follow/getdents64,
close_range and helper-local subreaper support. Descriptor cleanup unavailability
refuses launch. A dedicated helper owns the shell's distinct process group,
adopts inherited-group descendants and reaps every owned child before publishing
its report. Host signal disposition/subreaper/cwd are untouched. The shell resets
signal masks/dispositions and receives only configured environment/stdio.

The parent only performs bounded nonblocking pipe reads and WNOHANG checks.
Cancellation/deadline queues control work; each poll attempts at most one
nonblocking TERM/KILL send. EINTR/EAGAIN retain a pending command for a later poll.
Accepted TERM is sent only once; the original KILL deadline is never extended by
retry or repeated cancellation. At that deadline an unsent TERM is superseded by
KILL, whose transient send failures also retry through later polls. Cleanup
continues capture draining and waits for the helper's final report and reap.
A shell exiting before its grandchildren does not complete the execution.
Missing/unverifiable status is INDETERMINATE, never a successful reap claim.
Descendants must remain in the inherited process group; deliberate daemonization
or group escape is outside the supported cancellation profile. Permission to run
a command is **not OS confinement**.

## Executable evidence

`test_toolkit`, `test_toolkit_critic`, `test_toolkit_parallel`,
`test_toolkit_async`, `test_workspace`, `test_toolkit_resources` and
`test_toolkit_fencing` and `test_toolkit_review_faults` link the real
core/session/store libraries. Dedicated
fixtures cover strict schema/Unicode/depth, policy, copy overflow, one effect,
constructor OOM, runtime allocation refusal, bounded worker creation failure,
workspace denial/traversal/symlinks, copied environment, process truncation,
timeout/cancel, transient TERM/KILL send failures, persistent backpressure,
TERM-resistant descendants, edit fsync refusal and complete reap. Files/processes
are created only in disposable test workspaces. Integration with the real client
and independent review remain separate Stage 14 gate work.
