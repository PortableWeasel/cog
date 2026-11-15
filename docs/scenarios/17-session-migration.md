# Scenario 17: Quiescent same-host migration with durable fencing

Stage 14.D pass-2 retrace from `29b41e8`, amended here. This replaces the
historical SVG. Migration between execution owners is distinct from the v1→v2
format migration in [R10](R10-persistence-migration.md).

## Sequence diagram

```mermaid
sequenceDiagram
    participant Coordinator
    participant Source
    participant Store
    participant Target
    participant Router
    Coordinator->>Source: CGH1 QUIESCE(expected,next,checkpoint)
    Source->>Source: stop input; cancel/drain/reap provider/tools/children
    Source->>Store: cog_store_checkpoint_save(store,session,guard,checkpoint)
    Source->>Store: cog_store_owner_release(guard); remain FROZEN
    Coordinator->>Target: exact shared-store checkpoint token (STAGED)
    Target->>Store: cog_store_checkpoint_load(store,id,token,REPLAY,session,report)
    Target-->>Coordinator: CGH1 PREPARED echoing owner/checkpoint tuples
    Coordinator->>Store: cog_store_owner_compare_exchange(expected,next,checkpoint)
    Coordinator->>Target: CGH1 COMMIT exact tuples
    Target->>Store: cog_store_owner_acquire(store,id,next,guard)
    Coordinator->>Router: cog_orchestrator_publish_owner(session,target,next,checkpoint)
    Target->>Target: activate under validated current guard after route publication
    Source->>Source: release old tree; never resume under old epoch
```

## Exact public-header coverage

| Trace step | Exact declaration / member | Declaring header |
|---|---|---|
| Durable identity and exact generation | `cog_store_owner_t`; `cog_store_checkpoint_t`; `cog_store_config_t.require_owner` | [session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| Confirm current committed facts | `cog_store_owner_read`; `cog_store_load_report_t.checkpoint_generation` | [session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| Guarded quiescent checkpoint | `cog_store_checkpoint_save`; `cog_store_checkpoint_load`; `cog_store_owner_guard_t` | [session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| Delete/teardown refusal | `cog_store_delete`; `cog_store_delete_fn`; `cog_store_free` | [session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| Freeze/commit/activate | `cog_store_owner_release`; `cog_store_owner_compare_exchange`; `cog_store_owner_acquire` | [session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| Tool guard lifetime | `cog_store_owner_guard_borrow`; `cog_store_owner_guard_return` | [session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| Quiesce model work | `cog_provider_cancel`; `cog_provider_next`; `cog_provider_disconnect` | [provider_vtable.h](../../libraries/cog-provider/include/cog/provider_vtable.h) |
| Quiesce/reap/free execution borrowers | `cog_toolkit_execution_cancel`; `cog_toolkit_execution_poll`; `cog_toolkit_execution_free` | [toolkit_exec.h](../../libraries/cog-toolkit/include/cog/toolkit_exec.h) |
| Correlated portable messages | `cog_handoff_message_t`; `cog_handoff_kind_t`; `cog_orchestrator_handoff_encode`; `cog_orchestrator_handoff_decode` | [orchestrator.h](../../libraries/cog-orchestrator/include/cog/orchestrator.h) |
| Advisory route publication/query | `cog_orchestrator_publish_owner`; `cog_orchestrator_session_owner`; `cog_orchestrator_forget_owner` | [orchestrator.h](../../libraries/cog-orchestrator/include/cog/orchestrator.h) |
| Bounded transport and peer service | `cog_ipc_endpoint_send`; `cog_ipc_endpoint_service`; `cog_ipc_endpoint_recv` | [ipc.h](../../libraries/cog-ipc/include/cog/ipc.h) |
| Explicit final tree release | `cog_session_free` | [session.h](../../libraries/cog-core/include/cog/session.h) |

## Ownership and failure branches

- Supported profile: Linux same-host processes share one verified store authority and immutable checkpoint files. Active transfer, cross-host migration and copying live provider/tool/guard handles are UNSUPPORTED. All source effects/process groups must be terminal and reaped before checkpoint/release; unresolved tools refuse ordinary migration.
- The active owner holds an independent-open-description exclusive OS guard for its entire execution lifetime. Two store contexts in the same process also contend. No process-global lock table or process-scoped fcntl shortcut is allowed. Guard descriptors never leak to spawned commands/aux processes.
- Source freezes before releasing its guard. Target validates the **exact** checkpoint and stages without effects. CAS takes the same lock, checks current owner and current immutable generation, durably publishes the next epoch, then releases. Target acquires that durable owner guard but remains staged without new writes/effects until initial route publication succeeds. It then activates; route publication or a COMMIT message alone never grants execution.
- Target failure before commit leaves source frozen. Abort/resume advances epoch even when returning to the same source incarnation; no old PREPARED/COMMIT ACK can later activate that attempt. Lost ACK is resolved by exact durable owner comparison and guard acquisition, never guessed rollback.
- Publish/fsync uncertainty is INDETERMINATE. Neither side resumes on an assumption. Identical CAS retry confirms durability; `owner_read` is diagnostic, not an activation grant. After commit, route failure leaves the new durable owner authoritative; retry route publication, never restore the old owner from a stale ACK.
- Restart consults durable owner/checkpoint, fences/reaps prior owned processes and admits a fresh incarnation/epoch. STARTED or unresolved effect intent remains INDETERMINATE and cannot be replayed automatically. OS lock release on process death is not proof that descendant side effects completed.
- Deletion through public delete or backend delete_ refuses any durable owner record or ownership lock anchor with UNSUPPORTED, even after guards release or via an unguarded context. Refusal is serialized against bootstrap/adoption; no unlink/recreate can reset fencing identity. Store destruction requires all guard borrowers drained and guards released first.
- Every CGH1 message echoes session, expected/next owner, source/target peer and exact generation. Wrong peer, stale epoch or changed checkpoint ACK is rejected. No native pointer struct or arbitrary store path crosses IPC.

## Gap amendment

**O17-G1 / D14 resolved at contract level:** public guarded checkpoint, owner CAS,
portable handoff and advisory route APIs replace nonexistent serialize/deserialize
helpers. No unresolved declaration is deferred to N. G supplies persistence and
guards; N/O supplies same-host protocol/orchestration against this contract.
Pass 3 must independently review this profile and its acquisition/precondition
boundaries before implementation is approved.

## Executable evidence

Existing baseline: [test_17_session_migration.c](../../applications/cog-cli/tests/test_17_session_migration.c).
It exercises historical mock composition and was not rerun in this header-only
pass; it does not prove the new fence/guard contract.

Required future tests: source/target failure at every checkpoint, release, CAS,
fsync, guard acquisition and route publication boundary; stale ACKs and duplicate
commit; same-process competing contexts; delete attempts with live/released guards and concurrent owner bootstrap/adoption; exact-generation refusal; old incarnation
restart; live child/unresolved ledger refusal; byte-exact opaque continuation and
no duplicate effects. No passing runtime result is inferred from this trace.
