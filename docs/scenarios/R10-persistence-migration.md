# R10: Save, restart, version migration and safe recovery

Stage 14.D pass 2; baseline header pass 1 `29b41e8`, amended by this pass.
This is a contract trace, **not an executable conformance pass**.

## Sequence

```mermaid
sequenceDiagram
    CLI->>Store: cog_store_save(store,session)
    Note over Store: fsync complete generation -> atomic manifest -> directory fsync
    CLI->>Store: cog_store_load_ex(store,id,REPLAY,session,report)
    alt v1 text-only
    Store-->>CLI: migrated_in_memory=true; source_version=1; no disk write
    else v1 unidentified tool history
    Store-->>CLI: UNSUPPORTED (INSPECT only, replay_safe=false)
    else v2 outstanding STARTED
    Store-->>CLI: INDETERMINATE; ordinary history refuses replay
    end
    CLI->>Session: cog_session_copy_history(session,node,msgs,capacity,count)
```

## Exact public coverage

Each symbol below is declared in the linked public header. Local application
decisions in the diagram are not invented library functions.

| Step | Exact calls, types, events or fields | Declaring header |
|---|---|---|
| Version-aware operations | `cog_store_delete`, `cog_store_delete_fn`, `cog_store_free`, `COG_STORE_FORMAT_V2`, `cog_store_save`, `cog_store_load_ex`, `cog_store_load_report_t`, `COG_STORE_LOAD_REPLAY`, `COG_STORE_LOAD_INSPECT` | [libraries/cog-core/include/cog/session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| Recovery policy | `cog_store_config_t`, `cog_store_append` | [libraries/cog-core/include/cog/session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| History refusal/ledger | `cog_session_copy_history`, `cog_session_tool_state`, `COG_TOOL_EXECUTION_INDETERMINATE` | [libraries/cog-core/include/cog/session.h](../../libraries/cog-core/include/cog/session.h) |
| Exact structured content | `cog_message_t`, `cog_message_opaque_t`, `cog_usage_t` | [libraries/cog-core/include/cog/message.h](../../libraries/cog-core/include/cog/message.h) |


## Ownership and invariants

- v1 already exists; this is v1->v2 migration, not initial versioning. Text-only migration is lossless in memory; explicit save publishes v2. Never invent call IDs/effects for v1 tool records.
- v2 atomically commits tree identity, associated results and ledger, usage availability and exact opaque bytes. No independent node/index publication. Unknown versions/unsupported custom backend fail UNSUPPORTED.
- Tail recovery may discard only an unterminated record AFTER the manifest-validated committed prefix. Missing committed bytes/results/STARTED, middle corruption or malformed complete records fail. It must not erase an effect by calling it a tail.
- Failed directory durability sync means IO/uncertain, never permission to execute after STARTED. Crash after a real effect but before final result publication loads INDETERMINATE without rerun.

## Failure and capacity branches

- Owned-session or ownership-anchor deletion returns UNSUPPORTED through both public and backend paths; require_owner=false is not a bypass. Race deletion against bootstrap/adoption and verify the stable anchor is never unlinked/recreated. Free the store only after all guards/borrowers are released.

- Fixtures must include v1 text, v1 tools inspect/refusal, v2 complete, unknown version, STARTED, unknown opaque namespace, missing derived index and each atomic-publish crash point.
- INSPECT is not a bypass for provider_send or tools. It allows observation only; replay_safe=false is propagated.

## Evidence and gap disposition

Original13/17 baseline tests and current session_store.c use v1; they do not prove v2 migration/recovery.

**Required future executable evidence:** G/L: byte-exact opaque fixture save/load/fork; generation crash fault injection; malicious association and committed-prefix truncation tests.

**Amendment references:** D07,D08 in [pass-2 register](../REAL_CLIENT_SCENARIO_PASS2.md).

## Pass-3 recovery retrace (E01)

A crash leaves STARTED as INDETERMINATE. Ordinary guard acquisition still refuses
execution. After fencing/reaping the old incarnation and admitting a fresh epoch
against the exact latest checkpoint, an explicitly authorized operator uses the
restricted recovery path below; there is no automatic task or tool replay.

```mermaid
sequenceDiagram
    App->>Store: cog_store_owner_acquire_recovery(id,current_owner,guard)
    App->>Session: cog_session_reconcile_tool(assistant,id,resolution,identified_result)
    App->>Store: cog_store_save(reconciliation only)
    App->>Store: cog_store_owner_release(recovery_guard)
    App->>Store: owner_read; owner_compare_exchange(NEW epoch,exact checkpoint)
    App->>Store: cog_store_owner_acquire(normal guard) only after safe validation
```

| Recovery step | Exact declaration | Declaring header |
|---|---|---|
| Restricted ownership | `cog_store_owner_acquire_recovery`; `cog_store_owner_guard_borrow`; `cog_store_save` | [session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| Atomic explicit result closure | `cog_session_reconcile_tool`; `COG_TOOL_EXECUTION_ACKNOWLEDGED` | [session.h](../../libraries/cog-core/include/cog/session.h) |

Recovery guard cannot admit model/tool/child work or ordinary append/checkpoint.
Only explicit monotonic ledger/result reconciliation is saveable; all original
messages, opaque bytes and audit evidence remain. ACKNOWLEDGED preserves unknown
effects in its result instead of claiming they did not happen. A failed save or
crash retains uncertainty; it never repeats execution. Releasing recovery guard
does not itself grant execution: a new durable epoch and normal guard are required.
Future G/L tests must cover rejected execution borrows, wrong call IDs, capacity
failure atomicity, failed final save and crash after every recovery boundary.

## Independent final-review revision: committed identity (E08)

Discard/reset cannot remove identified/admitted/effect-bearing/fork-shared state.
Safe disposal remains limited to uncommitted text drafts, without rewinding node
IDs. Save/append compare the locked committed generation even if the caller
constructs an empty replacement with the same session ID: committed call/assistant/
ancestor identity, arguments, opaque data, results and prior audit evidence cannot
be removed/rebound or rolled backward. Only documented monotonic ledger progress
is permitted; final results are immutable and INDETERMINATE closes only through
explicit recovery. Newly indeterminate work halts new admission under an already
held guard as well as during acquisition. Failed/uncertain save never makes prior
intent erasable. Test discard/reset/replacement-save against a saved STARTED leaf
with no result, and against a completed result/audit; expect refusal and zero reruns.

## Independent final-review revision: active path and unstarted work (E09/E11)

While any call/result is unresolved, `cog_session_fork` and arbitrary-parent
`cog_session_add_message` cannot switch to a safe sibling or ancestor. Only a
matching TOOL result at the active leaf, or atomic reconcile_tool, may extend
that path. REPLAY load rejects off-active unresolved state; no recovery target
can be stranded. An incomplete text-only turn without tool ledger still permits
an explicit fork from its safe ancestor. Crash oracle: create a safe sibling,
then save active STARTED; attempted sibling switch must fail before the crash,
and restored INDETERMINATE remains reachable for reconciliation.

PROPOSED is unresolved lifecycle state but proves no STARTED admission happened:
normal guard acquisition may resume that current policy/denial phase. It cannot
start unrelated model/user/branch work. Crashed STARTED becomes INDETERMINATE and
requires recovery. Cancelled unstarted proposals receive identified no-effect
DENIED results; they are never mistaken for uncertain executed effects. Test the
crash before STARTED save and cancel-after-proposal boundaries with zero effects.

## Independent final-review revision: public exact restore (E10)

Custom save callbacks obtain complete records with `cog_session_snapshot_view`;
load callbacks return a session through `cog_session_restore`, without private
layout, ID setters or dummy-node burning. This includes sparse IDs, parent/sibling
order, selected active leaf, node/audit high-water, exact identified ledger and
audit/evidence bytes. STARTED conservatively normalizes to INDETERMINATE.

| Public persistence boundary | Exact declaration | Declaring header |
|---|---|---|
| Complete bounded records | `cog_session_snapshot_t`; `cog_tool_ledger_record_t`; `cog_tool_audit_record_t`; `cog_session_snapshot_view` | [session.h](../../libraries/cog-core/include/cog/session.h) |
| Explicit identity restore | `cog_session_restore`; `cog_session_snapshot_t.active_node_id`; `cog_session_snapshot_t.node_id_high_water`; `cog_session_snapshot_t.audit_high_water` | [session.h](../../libraries/cog-core/include/cog/session.h) |

Snapshot arrays are borrowed and must be encoded/copied before mutation; restore
transitively copies them into explicit constructor reservations. A larger restore
may retry recovery LIMIT without changing identity/history or growing in place;
keep the old object until construction succeeds. Test sparse IDs/active branch,
wrong high-water, duplicate IDs, malformed audit chain, custom-context round trip
and a last-node STARTED snapshot restored with enough room to append recovery.

## L application entry retrace

The header-only L amendment adds `cog_cli_options_t.session_action` with explicit
RESUME/RECOVER selection and `source_session_id`. RESUME loads an exact checkpoint,
restores current constructor reservations, CAS-admits a fresh incarnation/epoch
and acquires a normal guard. Text-only v1 explicitly migrates to unowned v2 first;
unsafe legacy tools refuse. PROPOSED resumes only the current policy phase;
STARTED/INDETERMINATE returns INDETERMINATE without model/tool execution.
RECOVER requires the operator's fencing assertion and a recovery guard, without
credentials/net/core. `cog_cli_app_reconcile` closes exactly one selected node/call
with explicit evidence/outcome and saves atomically; it never grants ordinary
execution. Later explicit RESUME advances to another normal epoch. See
[three-pass amendment](../REAL_CLIENT_L_CONTRACTS.md). Executable evidence remains L work.
