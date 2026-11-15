# Structured session implementation contract

Stage 14.G implements the public `cog/session.h` contract. The constructor
reserves nodes, a coalescing payload arena, ledger/audit descriptors, public
snapshot records and history scratch. Append, fork, discard, transition,
reconciliation and history do not allocate or grow their reservations.

A complete structured message is copied transitively into one node payload.
Content and argument lengths are explicit; opaque bytes retain embedded NUL.
Neutral message validation checks UTF-8, role, pointer/count pairs, call-ID
uniqueness and usage availability. JSON/schema and opaque namespace support
belong to the consuming provider/toolkit.

Node and audit identities are monotonically issued, including discarded drafts.
Capacity exhaustion fails before mutation; there is no automatic pruning.
Discard/reset only remove uncommitted text drafts and cannot erase identified
tools, results, audit, restored nodes or shared ancestors. Constructor failures
release every reservation. Restoring with a larger reservation is a separate
constructor and leaves the original object alive.

The execution key is assistant-node identity plus provider call ID. Every call
starts PROPOSED. Only documented compare-and-transition steps add immutable
audit records. A terminal state and its single associated COMPLETE TOOL result
are required for replay. Unresolved entries block unrelated input and branch
switches session-wide; INDETERMINATE permits only explicit atomic reconciliation.
Reconciliation records copied result text as immutable evidence and consumes
node, payload and audit capacity atomically.

Fork returns the same immutable shared node, preserving ancestor effect identity.
Selecting a nonleaf leaves the current active leaf selected until explicit
`add_message(parent=fork_point)` creates a divergent child. Leaf selection can
select that leaf. No copying of assistant identities or tool execution occurs.

Public snapshots preserve preorder sibling order, sparse IDs, active leaf,
identity high-water, all structured and legacy fields, usage and complete audit
records. Restore validates parent order, duplicates, ledger/audit chains, global
audit identity uniqueness, result associations and active-path unresolved state.
STARTED conservatively adds one INDETERMINATE transition; restoring that result
again does not add another transition. Unsafe unidentified legacy tools require
inspection mode, which cannot build provider history or mutate execution state.

`tests/unit/test_session.c` covers capacity/discard identity, arena reclamation,
transitive copying, unknown/duplicate associations, crash normalization,
capacity-failed reconciliation, larger restore, immutable audit evidence,
branch-local call-ID reuse, immediate nonleaf-fork snapshots, off-path unresolved
refusal, sparse restore, malformed identities/audits and legacy inspection.
`test_message.c` covers malformed UTF-8, duplicate IDs, pointer pairs, roles,
opaque bytes and finite known cost. Targeted strict C11 and ASan/UBSan executions
pass. Store migration, durable generations and OS ownership are the separate
G.2 persistence contract; they are not inferred from these session tests.

The private allocation-free `cog_session_snapshot_validate` checks exact public
records for store admission without constructing a hidden temporary session.
It validates tree/active-leaf identities, complete call/result associations,
audit chains and unique sequences, preserved high-water, and the prohibition
on unrelated descendants after unresolved calls. It does not normalize STARTED
or grant execution. Restore calls this validator before reserving its owned
object, then copies and conservatively normalizes.

`test_session_allocations.c` uses linker wrappers for malloc/calloc/realloc/
strdup: every constructor allocation is failed individually for new and restore,
and all append/discard/transition/reconcile/history/fork/snapshot/validation
operations run with allocation forbidden. Strict and ASan/UBSan runs pass.

## Independent G.1 review revision 1

Completion is branch-local as well as session-wide. A finalized ledger entry
and TOOL result in a sibling branch cannot complete the selected provider
context. Fork/history/new-message insertion require exactly one associated final
result for every assistant call on the selected parent path. Partial multi-call
result prefixes are unsafe fork points. Snapshot validation checks each retained
leaf independently, allows unresolved assistant turns only on the active leaf,
and refuses unrelated messages before their parent path's tool results. A raw
wire decode of such a malformed branch remains inadmissible in REPLAY or INSPECT.

Discard of an active divergent text draft refuses INVALID_STATE atomically when
its parent retains another child. Successful discard continues to select its
parent, which is then a leaf. Refusal preserves node/payload capacity, selected
leaf, sibling order and high-water. An unrelated uncommitted text draft can still
be discarded while retaining the selected leaf.

Regressions cover assistant-before-result fork/add/history refusal, incomplete
multi-call prefixes, active and off-active malformed sibling snapshots,
serialized malformed branch rejection including inspection mode, and text-only
divergent discard. All four targeted message/session/codec/allocation tests pass
strict C11-Werror-NDEBUG and ASan/UBSan. Independent re-review remains required.

## Independent G.1 review revision 2

Explicit legacy inspection insertion uses its own private admission mode. It
permits traversal of unidentified legacy call/result nodes when constructing the
documented call -> result inspection chain, while retaining all identified
ledger/result path checks. Ordinary structured insertion, fork and history use
strict replay admission. The inspection helpers do not invent tool IDs, ledger
entries, audits or replay authority; known unresolved identified effects and
sibling-only identified results remain rejected.

Positive regressions construct call/result chains both at an empty root and
below a text root, verify transitive tool-name/arguments/result copies, retain a
zero-entry ledger, refuse history/ordinary append/fork, and restore the exact
records only in inspection mode. The earlier branch-local and divergent-discard
regressions remain passing. All four suites plus the independent legacy
reproduction pass strict C11-Werror-NDEBUG and ASan/UBSan (ten executions).

Independent review accepted G.1 and the final G.2 commit bookkeeping at
`619fa0a`; merge `3c4e39c` preserves the reviewed history. Combined core/store
and scenario 13/14 evidence, including the remaining application/release limits,
is recorded in `../session_store/CONTRACT.md` and ticket `0a33dd7`.
