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

The current owner supplies `app->opts`, `app->provider` and its immutable generation.
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
6. At a safe idle boundary before next input, install future aliases and update
   the core reference without calling user code in between. Disconnect old before
   freeing/clearing its generation. Service and activation allocate nothing.

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
