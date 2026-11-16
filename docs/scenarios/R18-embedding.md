# R18: Install, external provider, independent net contexts and cleanup

Stage 14.D pass 2; baseline header pass 1 `29b41e8`, amended by this pass.
This remains the contract trace. The bounded permanent installed-consumer
fixture below supplies scoped executable evidence; it does not close the full
release gate or pending O runtime coverage.

## Sequence

```mermaid
sequenceDiagram
    Host->>Host: explicit curl/dependency global initialization
    Host->>External: cog_provider_connect(endpoint_with_static_vtable,provider)
    External->>External: first-member cog_provider_t base initialized before publication
    Host->>Provider: cog_provider_preflight(provider,request,COMPLETE)
    Host->>Provider: cog_provider_send(provider,request); poll/next/cancel
    Host->>Provider: cog_provider_disconnect(provider)
    Host->>Store: cog_store_new(A,backend_contextA); new(B,backend_contextB)
    Host->>Store: save/load via public snapshot_view/restore
    Host->>Store: cog_store_free(B); caller destroys contextB
    Host->>Store: A still usable; free(A); caller destroys contextA
    Host->>Net: cog_net_new(netA,explicit_profile_config)
    Host->>Net: cog_net_new(netB,explicit_profile_config)
    Host->>Net: construct independent request IDs; cancel/release/free netB, then netA
    Host->>Host: dependency cleanup after all non-Cog users also finish
```

## Exact public coverage

Each symbol below is declared in the linked public header. Local application
decisions in the diagram are not invented library functions.

| Step | Exact calls, types, events or fields | Declaring header |
|---|---|---|
| Public external base/dispatch | `cog_provider_t`, `cog_provider_vtable_t`, `cog_provider_connect`, `cog_provider_preflight`, `cog_provider_preflight_stage_t`, `cog_provider_send`, `cog_provider_poll`, `cog_provider_next`, `cog_provider_cancel`, `cog_provider_get_error`, `cog_provider_disconnect` | [libraries/cog-provider/include/cog/provider_vtable.h](../../libraries/cog-provider/include/cog/provider_vtable.h) |
| Immutable composition | `cog_endpoint_t` | [libraries/cog-provider/include/cog/endpoint.h](../../libraries/cog-provider/include/cog/endpoint.h) |
| Explicit lifecycle/memory contract | `cog_net_new`, `cog_net_free`, `cog_net_config_t`, `COG_NET_DEPENDENCY_MEMORY_DYNAMIC` | [libraries/cog-net/include/cog/net.h](../../libraries/cog-net/include/cog/net.h) |
| Caller-owned custom backend | `cog_store_config_t.backend_context`, `cog_store_new`, `cog_store_free`, `cog_store_save`, `cog_store_load_ex`, `cog_store_vtable_t` | [session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| OpenRouter net usage requirements | `cog_openrouter_config_t`, `cog_openrouter_provider_vtable` | [libraries/cog-provider/include/cog/openrouter.h](../../libraries/cog-provider/include/cog/openrouter.h) |


## Ownership and invariants

- External provider embeds cog_provider_t FIRST and uses only installed public headers. All nine static callbacks are required; no src/provider_internal.h dependency or guessed memory prefix.
- Host validates actual shipped c-ares-only provenance (ENABLE_ARES=ON/ENABLE_THREADED_RESOLVER=OFF), runtime ASYNCHDNS/ares and DNS_SERVERS probe. Threaded/hybrid/unknown builds fail UNSUPPORTED. No QUICK_EXIT abandoned thread workaround.
- Two net contexts have independent slots, IDs, rings and cleanup. Freeing A cannot invalidate B, and never calls dependency-global cleanup. Shared context providers use id-local peek/ack, not accidental global draining.
- DYNAMIC explicitly permits third-party handle-owned service-time allocation; no total memory bound is promised. Nonzero dependency_memory_limit is UNSUPPORTED. Gate E must accept/reject this ownership profile.

## Failure and capacity branches

- Mixed/reverse public-header standalone C11/C++17 compilation and out-of-tree static-vtable consumer must pass. Installed provider target must export the new net usage requirements; core retains only forward dependencies.
- Allocation/backend failure unwinds local handles without cleaning the host runtime. Maintained shipped dependency build repeats the controlled DNS cancellation suite; research versions are not permanent version approval.

## Evidence and gap disposition

The permanent [installed C11 consumer fixture](../../tests/external-consumer/README.md)
uses a full normal install, public target imports and two prefix relocations.
It covers a bounded text-only nine-callback external provider/preflight, two
independent custom store contexts, explicit host dependency lifetime and reverse
two-net cleanup, exported module link closure and basic mock CLI lifecycle.
Final acceptance is tracked by `8f13685`/`b61fb1b` and requires the separately
reviewed installable dependency profile `ba50799`; the old development artifact
with foreign RUNPATH is not shipped acceptance.

**Remaining release evidence:** full original20/R01-R18, standalone C++ header
checks, broader exact-restore edge cases and pending O async/handoff/runtime
implementation remain separate. A passing scoped consumer does not close P.

**Amendment references:** D02,D10 in [pass-2 register](../REAL_CLIENT_SCENARIO_PASS2.md).

## Pass-3 custom store construction retrace (E06)

An out-of-tree store backend constructs its own caller-owned context, supplies
`cog_store_config_t.backend_context`, and receives that exact pointer in every
custom callback ([session_store.h](../../libraries/cog-core/include/cog/session_store.h)).
No callback casts a private Cog wrapper or uses a mutable global. The caller
keeps context alive until all guards are released and store_free has finished,
then invokes its own matching destructor. Generic store_free never frees this
borrowed state. Missing required callbacks or incomplete fenced/recovery groups
fail before publication. The permanent C11 fixture uses two independent custom
contexts and asserts callback identity, public snapshot/restore ownership and destructor ordering.
The broader G/P restore/ownership edge-case inventory remains separate.

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
