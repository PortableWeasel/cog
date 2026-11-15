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
LIMIT. Initial pointer/presence/resolver-combination, mode/state/busy refusals
do not count; candidate string/byte/constructor/compatibility/commit failures do.
Startup configuration ID is1;
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


## Pass 3 — consistency and final self-review

The third actual pass cross-checked all twenty targeted rows, the original20
mapping and public/private ownership/error declarations. Amendments in this pass:

- Activation returns AGAIN when a future owner exists but old event/result/tool
  state is not safely drained. Input handlers must defer without freeing or
  reassigning their reserved input identity; they cannot quietly start another
  old-generation turn. No new allocation or event-lease bypass is introduced.
- An explicit terminal reload is a visible constructor operation; the surrounding
  ordinary service/activation path still cannot allocate. Credential source
  buffers/callback authority are borrowed only for that operation; the copied
  resolved secret necessarily remains in its new owner until disconnect/free.
- Terminal helper return semantics distinguish a handled sanitized candidate
  refusal (OK, continue old work) from output IO. The public API still returns
  the original candidate error with its output generation untouched.
- Known selected history uses the pending fork point when present, otherwise
  the selected active path. A genuinely unsafe currently selected prefix refuses;
  no sibling inspection, fabricated result, opaque dropping or history rewrite.
- Failed/cancelled old work must drain and retain its fresh-input rule. Ordinary
  diagnostic continuation_required is not a new execution authorization; actual
  indeterminate/frozen result admission cannot activate a future owner.

### Exact fixed decisions and error ordering

Required NULL pointers fail NULL_PTR before dereference. Then validate the
three-field presence/pointer combinations (INVALID_ARG or fixed MOCK UNSUPPORTED),
mode/shutdown/cancel/reentrant state (INVALID_STATE), actual frozen admission
(INDETERMINATE), future/retiring/notice occupancy (AGAIN), and attempt capacity
(LIMIT), before reserving an attempt or invoking constructors/resolvers. String syntax/
byte validation belongs to candidate construction after that reservation; an
INVALID_ARG/LIMIT there consumes an attempt. Once reserved its ID is burned even
if candidate work fails. Candidate
resolution/provider/preflight/config errors return directly after safe rollback.
No failed call changes active/published config, input lease, current options,
policy decision, old request/error/clock/delta or session/guard state; only its
bounded attempt/transaction tombstone counters advance. On success
out_generation changes once and the candidate is published; activation is later.

The three control keys mirror the exact published reload subset, seeded from
startup effective values (absent optional MOCK values need no invented secret).
The tree is private, retains no caller transaction handles and stores no raw
credential. Its static validator checks that fixed key/value profile only;
credential resolution/provider construction occur before commit, not in a
validator callback. Reset-LIMIT finalization is handled without reusing a stale
transaction. App construction/free own the new_ex/tree_free pair. Repeated
reload is intentionally bounded to64 attempts, with no tree rotation/global ID
counter/unbounded tombstone growth. IDs are per-app, not a claimed process-global
identity scheme; fresh apps start at1 after prior owners are gone.

All nonreloadable fields remain byte/value-equivalent effective options and
retain existing borrows; no original prompt/queue/session action is resubmitted,
no constructor action/store admission is rerun. Callback authority is not
recovered from effective J options, which intentionally erased it. Host net and
app-wide obs context outlive both provider generations and final cleanup. Static
vtable/model pointers belong to their own immutable config owner. The new core
setter neither validates providers nor gains a provider implementation link;
caller proves its serialized borrow/retirement boundary. Provider getters expose
active state, not the future pending pointer, and must be reacquired after app
mutation/drive. Valid supported provider disconnect is infallible by its existing
contract; an unexpected violation freezes admission and must not falsely free
still-borrowed secret storage or claim transactional rollback.

### Mandatory executable oracles after independent header approval

- Actual two-provider app composition, normal and restored-prefix request IDs,
  old-turn retry/tool/policy pinning, next-turn new model/generation and stable
  original prompt/queue identities; core and app getter equality around switch.
- Credential callback lifetime/one-call resolution, unchanged omitted secret,
  mutation/free of caller source buffers after return, rollback secret clearing
  after candidate disconnect and old clear only after old disconnect.
- Constructor OOM and real config validator/commit rejection including reset-LIMIT;
  no publication on failure, exact previous running tree/provider still usable,
  neither wire request nor durable STARTED/session save from reload itself.
- Real provider no-wire preflight over supported/malformed/unsupported selected
  opaque plus pending-group histories; compatible unknown bytes stay lossless.
  Fake identity descriptors cannot establish this oracle.
- Old next OVERFLOW and terminal/error/cancel/clock snapshots survive preparation;
  future output limit/opaque failure never replays an old effect or drops data.
- Saturated real core queue retains exactly one notification (source_id by value,
  no payload); duplicate/stale CONFIG_CHANGED self events cannot change request
  sequence/policy. Test a queued input before notice delivery and old stale event.
- Busy/reentrant/mode/NULL/error precedence and exact64-attempt/transaction limits,
  including failed attempts; no extra resolver/construction on refusals.
- Shutdown/EOF/frozen output paths release or preserve both owners safely with
  core references detached/freed first. Background service/activation allocate
  nothing; instrument explicit constructor boundaries separately.

Strict C11/C++ header self-containment, local links, SVG XML and diff checks are
only syntax/design validation. No source `.c`, executable/runtime, paid/network
or release evidence is claimed by this ticket. After this pass a DIFFERENT final
reviewer must inspect the complete candidate before source writing is authorized.


## Gated delivery strategy and exact file ownership

All source children wait for `dffc59e` independent PASS and integration. Their
parent is `af6956a`, already after completed L; M depends on that delivery. The
reviewed header checkpoint is the source contract, not authority to code from an
unmerged moving draft. No architecture/API extension is delegated to a writer.

| Child | Owned implementation and substantive checks | Dependencies / parallel rule |
|---|---|---|
| `b488e99` R12.1 | `libraries/cog-core/src/core.c`, `tests/unit/test_core.c`, core contract evidence: pointer setter NULL/same/new/detach with zero provider/destructor/allocator/state side effects | Header gate; parallel with R12.2 only |
| `55779c4` R12.2 | `applications/cog-cli/src/configuration.c`, `applications/cog-cli/tests/test_configuration.c`, `test_configuration_memory.c`: resolved clone, secret rotation/copy/clear, no precedence or authority retention | Header gate; no app lifecycle/CMake ownership |
| `881cb81` R12.3 | New `applications/cog-cli/src/app_reload.c`; `cli.c`, `app_turn.c`, `app_sessions.c`; app and app-test `CMakeLists.txt`; new `tests/test_app_reload.c` in app; `test_app_memory.c`; `tests/scenarios/test_08_config_reload.c`; app CONTRACT/HARNESS and narrow evidence | R12.1+R12.2; serial across all shared app/CMake owners |
| `faaf1ea` R12.4 | DIFFERENT independent critic, substantial missing boundary tests, strict GCC/Clang full builds, focused ASan/UBSan/LSan and affected concurrency; exact checkpoint verdict | R12.3; <=4 review-failure cycles |
| `7ad3bf9` R12.5 | Authorized merge, scoped postmerge evidence, installed CLI/static consumer dependency closure, recoverable cleanup and finalization | R12.4 PASS + root authorization |

R12.3 centralizes generation cleanup instead of leaving cli.c's old direct
provider/config frees to double-destroy aliases. It initializes the control tree
only for execution apps after startup config validity and handles all partial
constructor exits. Both normal send in app_turn.c and resumed pending preflight
in app_sessions.c use the pinned configuration ID; display generation is untouched.
App linkage adds `cog::config` and preserves installed static consumer closure.
CONFIG_CHANGED rows are static const self handlers in every normal app state;
no dynamic registration. Root M/P gates retain their full applicable checks;
scoped R12 oracles do not waive remote/headless O or final all38 coverage.
