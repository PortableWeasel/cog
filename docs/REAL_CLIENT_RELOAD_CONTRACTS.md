# Live application reload: bounded ownership contract

Design `dffc59e`, delivery `af6956a`; integrated L baseline `8c68b4a`.
This is header-first design only. Three real targeted passes and a DIFFERENT
independent final review must pass before any `.c` implementation.

## Pass 1 — goals, blocks, audit and smallest functional profile

Goals are to prepare a replacement without wire effects, atomically commit and
publish it, finish existing model/tool work with its original settings, select
the replacement only for the next idle turn, and retire every old secret only
after provider disconnect. Rollback must preserve the old usable generation.
Keep the public core/app provider views consistent and enforce compatibility
before effects; never confuse display incarnation with configuration identity.

Three blocks suffice: immutable J configuration owners plus a tiny app-owned
config transaction, the existing generic provider constructors/preflight, and
the app's serialized turn boundary. `cog_core_set_provider` is the minimal
borrowed-reference operation needed for the core getter; it does not call or own
providers. The [ownership diagram](live-reload.svg) maps each goal to these
blocks. No watcher, registry, refcount, new persistent format or backend switch.

The audit `501659c` found a one-owner constructor, hardcoded generation1 normal
send and restored-prefix preflight, no production CONFIG_CHANGED/reload handler,
and a core pointer retained from construction. Original08 tests config snapshots
and a local event handler only. J's two-generation test binds a zero-callback
identity descriptor and never connects a provider. Existing public config and
provider APIs already provide transactions, immutable copied views, no-wire
constructors and COMPLETE/PENDING_TOOLS compatibility; no PoC or external
research is needed to establish their feasibility. J drops credential resolver
authority after construction, so reload must receive that authority per call.

### Exact subset and ownership

`cog_cli_app_reload(app,replacement,&generation)` accepts only model ID, base URL
and credential reference. NULL retains the old effective value; empty is invalid.
REAL accepts all three, MOCK model only. A supplied credential reference resolves
again even if unchanged; omitted reference copies the already-owned secret.
Per-call callback/context are borrowed and discarded, never retained for service.
Unrelated options do not reread argv/environment/files. The callback or explicit
env: reference is the only new secret resolution. Backend/host net/static adapter,
store/guard/session/workspace, tool policy/limits, retry/output settings, fixture
script and reservations remain fixed. This is the smallest functional R12 subset,
not permission to retroactively alter old-call policy or move a durable owner.

The explicit reload operation constructs at most one candidate beside the current
owner, using the same finite J/provider constructor ceilings. The app reserves a
control tree (64 aggregate nodes, 65536 string bytes, 65 transaction identities,
one static validator) during app construction. It contains only `models.default`,
`providers.openrouter.base-url` and `providers.openrouter.credential-ref`, never a
resolved secret. Startup seeds one transaction. At most64 eligible reload attempts
consume the remaining identities; failures count and new attempts then return
LIMIT. Busy/argument/state refusals do not count. Startup configuration ID is1;
attempts consume IDs2..65 without reuse, even on failure. Display/owner epochs
and request serials keep their existing independent meanings.

A retiring slot keeps old ownership explicit until disconnect succeeds; current+
future or current+retiring are the only two-live-owner layouts. No third candidate
is allowed. The current owner supplies `app->opts`, `app->provider` and its immutable generation.
A committed future owner is separately published for next idle work; while it
exists, another reload returns AGAIN before any constructor or resolver. The
entire old application turn, including retry, policy and all tool/model rounds,
retains current options. Model/request generation must come from that pinned
configuration in both normal send and restored-prefix preflight. New generation
publication does not force a next turn, bypass fresh postfailure input or replay
an effect. Inactive siblings are not candidate wire history.

### Transaction and failure sequence

1. Check required pointers, explicit subset, mode, frozen/shutdown state and the
   single pending slot. Reserve one attempt/identity, begin a real config candidate
   and stage the three effective control values.
2. Construct copied effective configuration with `configuration_reload_new`, bind
   its fixed static adapter and construct an idle provider. No network request.
3. Generically preflight the known selected safe prefix, including pending final
   assistant group/results when present, with the candidate generation/model.
   Unsupported/malformed opaque and configured limits fail before publication.
4. Commit the app-owned config candidate. If preparation or commit fails, rollback
   (unless commit finalized a rejected-reset LIMIT), disconnect candidate before
   clearing its secret, and retain the original error. Old config/provider/turn,
   retained output, input, policy and session/guard are unchanged.
5. Commit success has only infallible pointer publication remaining. Return its
   configuration ID. No postcommit constructor, allocation or fallible enqueue.
6. Deferred from the reload call, at a safe idle boundary before next input, install future aliases and update
   the core reference without calling user code in between. Disconnect old before
   freeing/clearing its generation. Background service/activation allocate nothing;
   explicit reload constructors may be invoked at the terminal owner input boundary.
   Unexpected destructor failure freezes admission and retains the retiring owner;
   it is a driver failure after a committed publication, never a fake rollback.

Preflight checks only known bytes. An old active turn can still produce new opaque
or output after publication. Its old provider finishes normally. Later COMPLETE
send under the replacement revalidates that future history and may fail without
silently rolling back committed config, dropping opaque or replaying old effects.
An unsafe currently selected prefix refuses reload until explicit safe selection.

Terminal entry is `/reload model VALUE`, `/reload base-url VALUE` or
`/reload credential-ref VALUE`. The full remaining suffix is one value; no shell
or implicit multi-field parsing. The host API supports one atomic three-field
change. Commands never echo credential/reference/URL/model values and are handled
before policy answers, never as model prompts or ALLOW/DENY. Failed reload reports
a sanitized refusal and keeps the old loop usable; output failure remains IO.

### Headers and implementation dependency boundary

| Responsibility | Declaring contract |
|---|---|
| Explicit reload subset and admission | `applications/cog-cli/include/cog_cli/cli.h` |
| Reload effective config constructor | `applications/cog-cli/src/configuration.h` |
| Bounded owner slots/tree, activation and terminal routing | `applications/cog-cli/src/app_internal.h` |
| Borrowed core provider publication | `libraries/cog-core/include/cog/core.h` |
| Actual candidate/commit/rollback | existing `cog/config.h` |
| Generic constructor/preflight/send/disconnect | existing `cog/provider_vtable.h` |

The app adds config linkage; core still uses only its provider forward declaration
and gains no provider/net/toolkit implementation dependency. No `.c` change here.


## Pass 2 — targeted traces, discovered gaps and amendments

```mermaid
sequenceDiagram
  Host->>App: reload(model / URL / credential-ref)
  App->>Config: begin + stage effective three-key candidate
  App->>Generation: copied reload constructor, bind, provider_connect
  App->>ProviderNew: preflight(known selected prefix, stage)
  alt any prepare/commit error
    App->>Config: rollback if transaction still live
    App->>ProviderNew: disconnect before candidate secret clear
    App-->>Host: original error; old provider/turn unchanged
  else commit succeeds
    App->>Config: commit
    App->>App: infallibly publish future + retained notice flag
    App-->>Host: OK + new configuration ID
    ProviderOld-->>App: finish whole old model/tool turn
    App->>Core: set_provider(new) at safe idle admission boundary
    App->>App: switch aliases without arbitrary callback
    App->>ProviderOld: disconnect, then clear retiring secret
    App->>ProviderNew: later ordinary COMPLETE send revalidates future bytes
  end
```

| Trace | Exact boundary / oracle | Header coverage |
|---|---|---|
| 1. Idle update before queued input | reload returns published ID, next admission activates before send; core/app getters agree | cli.h reload, core.h set_provider |
| 2. Reload while TEXT is retained after next OVERFLOW | candidate uses separate provider/workspace; old same sequence/required/storage survives | provider_vtable.h next/preflight; app_internal.h owners |
| 3. Active retry/backoff | old model, credential and budget remain pinned for every retry; future provider idle | cli.h reload + existing K request/service contracts |
| 4. Active PROPOSED/STARTED tool group | preflight known final group using PENDING_TOOLS; old entire application turn completes all tool/model rounds | provider_vtable.h stages; cli.h reload |
| 5. Policy prompt plus /reload | recognize reload before answer parser; old ALLOW/DENY state unchanged, no command becomes prompt | app_internal.h reload_command |
| 6. Two rapid replacements | pending generation OR undelivered notice gives AGAIN before constructors/secret resolver; no superseding/leak | cli.h reload capacity |
| 7. Resolver refusal/malformed secret | callback authority only this call; old copied secret stays valid, temporary candidate wiped | configuration.h reload_new + cli.h credential fn |
| 8. New provider constructor NOMEM/UNSUPPORTED | no wire; rollback transaction, disconnect only successfully constructed candidate | provider_vtable.h connect/disconnect; config.h rollback |
| 9. Commit validator rejection/reset LIMIT | free candidate safely; detect finalized transaction before rollback; old running tree and provider unchanged | config.h commit/rollback |
| 10. Full event queue after commit | retained CONFIG_CHANGED flag retried fairly; by-value source_id, no payload lease; no postcommit failure/rollback | event.h + app_internal.h reload_service |
| 11. Input event ahead of notification | activation at safe input-handler entry before borrowing provider; notice is observational self-transition | core.h set_provider, app_internal.h activate |
| 12. Selected nonleaf fork | preflight actual next selected prefix (fork_parent when present), not unrelated active/inactive siblings | session.h node views; cli.h fork/preflight |
| 13. Known incompatible opaque/unsafe selected prefix | preflight returns before commit/effect; old generation usable; no dropping metadata | provider_vtable.h preflight; cli.h reload |
| 14. Future opaque/output becomes incompatible/oversize | old turn finishes; new COMPLETE send may refuse; no retroactive rollback or effect replay | provider_vtable.h send/preflight |
| 15. Failed/cancelled old turn | pending next generation never authorizes queued continuation; frozen result keeps both owners until drain/free | cli.h reload; existing K fresh-input/retention |
| 16. Stop/EOF while future pending | no activation/admission; cancel/drain old, discard idle future, core detached/freed before providers | cli.h free/stop; app_internal.h reload_free |
| 17. Repeated attempts/capacity exhaustion | ID1 startup, attempts2..65 burned on failure/success, 65th eligible call LIMIT; output untouched on failure | cli.h reload + config.h finite tombstones |
| 18. NULL/mode/reentrancy errors | required NULL first; metadata/shutdown/cancel/recursive refusal before callbacks; no resource/identity burn | cli.h reload |
| 19. Getter/retirement lifetime | stable old getter during turn; switch app/core before old disconnect; retain old secret on unexpected destructor failure | core.h setter/getter; cli.h provider getter |
| 20. Explicit unchanged/unknown fields | no all-NULL no-op, no hidden settings reread; callback without explicit reference invalid; MOCK URL/key unsupported | cli.h reload_options |

Each row uses the exact linked public/private declarations in pass1's coverage
matrix. No passing executable claim is made. Source must prove every row with
actual app/config/provider composition, not another identity-only descriptor.

This pass found four real contract gaps and amended the headers:

- A future generation may be committed before an existing queued input event;
  requiring no handler in progress for the core setter was too strict. An input
  handler may activate before taking its provider borrow; no other old borrow may
  survive the boundary. Core still performs pointer assignment only.
- Postcommit notification enqueue can fail on saturation. Publication now sets a
  reserved notice flag. Fair service retries CONFIG_CHANGED with source_id=new ID,
  NULL payload; every app state has a self-transition handler. Reload remains busy
  until its one notice is handled as well as future activation/retirement. This
  notification never shares the model event_queued flag or changes turn serials.
- Publication and activation are distinct. reload always returns after pointer
  publication without disconnecting current. Activation uses explicit retiring
  fields so an unexpected destructor failure cannot lose its secret owner. An
  idle input boundary performs only infallible alias/core updates then destruction;
  any destructor contract violation freezes subsequent admission, not rollback.
- A reload refusal in terminal input must not abort/drain the old active turn.
  All candidate errors produce only a checked sanitized refusal, whereas output
  write failure retains its existing fatal IO/cleanup behavior. This routing is
  recognized even at an old policy prompt and never grants an effect decision.

### Original01–20 consistency retrace

| Original scenarios | Effect / retained requirement |
|---|---|
| 01/02/12 | Whole old turn/retry budget pinned; future send revalidates |
| 03/07/09/10 | Remote orchestration unchanged; core borrowed pointer setter adds no provider link |
| 04 | Constructor reserves finite control tree, cleanup handles partial init |
| 05 | Shutdown drains old and discards future before borrowed net teardown |
| 06/15 | All tool rounds stay old; no current policy/ledger/guard changes |
| 08 | Actual candidate/provider preparation, commit, publication, notice and retirement specified |
| 11 | Retained delta/display identities unchanged; no reconnect protocol extension |
| 13/14/17 | Durable owner/session/branch identity fixed; only actual selected prefix preflighted |
| 16/18/19 | Candidate error is sanitized direct status, no wire or last-turn error replacement |
| 20 | Existing cancel/fresh-input/queued continuation rules remain authoritative |
