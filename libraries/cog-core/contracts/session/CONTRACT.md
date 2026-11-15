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
