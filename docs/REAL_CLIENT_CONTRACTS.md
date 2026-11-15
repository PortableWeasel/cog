# Stage 14.C public contracts: pass 1

> [Stage 14.E final review](REAL_CLIENT_PASS3.md) passed after the additional
> independent review of `8563f81`. The three header passes and final review are
> complete; implementation and executable conformance remain delivery work.

Ticket `5df3ba1`; based on integrated transport/protocol research A/B and DNS
follow-up `23ae6c8`. This is the initial public-header pass. No production C
implementation is delivered or claimed. Scenario pass 2 (`345a536`) must amend
these contracts from actual call traces; pass 3 plus independent final review
(`feb17b6`) must finish before implementation starts. Planning passes and
standalone compilation do not replace those gates.

## Boundaries and ownership

The eight libraries remain. Core owns scheduling, sessions and generic tool
descriptors; it does not link transport/provider/toolkit implementations.
Provider owns Chat Completions framing, protocol assembly and serialization;
net owns HTTP/TLS/DNS, retained bytes and transport handles. Toolkit owns
argument/schema validation and asynchronous execution; CLI owns permission
interaction, the single retry budget, limits and durable admission of effects.
JSON remains a private dependency: provider and toolkit reserve explicit parser
and writer pools independently; neither exposes yyjson types or default allocator
fallback. This extends the earlier provider-only JSON wording because toolkit
must independently validate arguments before execution.

The implementation tables remain static const. Configuration creates immutable
endpoint/model generations that reference those tables. Applications retain each
generation and its resolved secret until every borrowing instance disconnects.
They publish a fully validated replacement or keep the old generation on reload
failure. No Cog registry, global allocator pointer or reference counter appears.

| Owner | Explicit construction | Owned storage | Matching release |
|---|---|---|---|
| Host | curl global/dependency init | Process-scoped dependency runtime | Host cleanup after all Cog and other users |
| Net context | `cog_net_new` | Slots, copied config, body/header rings, CURLM | `cog_net_free` |
| HTTP transfer | `cog_net_request` | Copied method/URL/headers/body, easy handle | `cog_net_request_release` |
| Provider | `cog_provider_connect` | Request copy, JSON pools, turn assembly, typed queue | `cog_provider_disconnect` |
| Session | `cog_session_new` | Bounded node/payload arena, ledger and legacy history scratch | `cog_session_free` |
| Store | `cog_store_new` | Bounded codec/read/write workspace | `cog_store_free` |
| Async tool | `cog_toolkit_execution_new` | Copied arguments/identity, schema pool, output, implementation handle | `cog_toolkit_execution_free` |
| Workspace | `cog_workspace_new` | Root descriptors, bounded file/traversal/edit storage | `cog_workspace_free` |

Constructors check size arithmetic and return NOMEM without leaked resources.
Runtime Cog paths do not grow these workspaces; exhaustion returns LIMIT with
an explicit failed/incomplete state, never silent history/output loss. Caller
output capacity overflow is a distinct recoverable delivery failure.

Third-party transport memory is an explicit exception to a *constructor-only*
allocation claim, not to ownership visibility: easy/multi/dependency handles
created by the named constructors may allocate during service and release that
memory during their matching destructors. Libcurl, c-ares, TLS and libc are not
bounded by Cog workspace capacities. `dependency_memory=DYNAMIC` must be
acknowledged; any requested nonzero total dependency memory ceiling is rejected
UNSUPPORTED. No production total-memory guarantee is inferred from the PoC's
curl-only counters. Gate E must review whether this declared constructor-owned
allocation profile is acceptable; a host requiring a total ceiling needs a
different backend. Do not install process-global allocator callbacks to pretend
that per-context quotas exist.

## Provider construction and progress

`cog_provider_t` is now a supported public first-member base containing the
static vtable pointer. An external implementation embeds it first and returns
`&impl->base`, so it never includes `src/provider_internal.h`. That private
header is only a compatibility include. The existing prefix is preserved;
adding mandatory callbacks is a source/ABI change to vtables, not a claim of
binary compatibility with pre-alpha clients.

`connect` reserves local state and returns without DNS or HTTP. `send` validates
and copies all nested request data before return and starts one asynchronous
turn. The application may immediately destroy input arrays/strings. One turn
per provider is in flight; the prior terminal delta must be consumed before
another send. Endpoint/config generation data remains borrowed until disconnect.

`poll(now,&deadline)` services a bounded nonblocking quantum. `cancel` detaches
wire work, retains accepted queued deltas and produces one ABORTED terminal
item unless completion is already queued. `get_error` returns sanitized stable
metadata until the next send/free. A terminal failure is represented by one
FINISH delta with its outcome; an infrastructure hook error propagates to core.
`retryable` alone never authorizes replay: accepted output and effects also
control `replay_safe`, and only the application applies its finite budget.

`next(delta,storage,capacity,&required)` copies exactly one retained typed item.
A short destination reports exact storage bytes including string terminators,
writes neither metadata nor storage, and retains the same sequence for retry.
This remains true across poll calls. A provider workspace ceiling violation is
instead terminal LIMIT. Full event queues pause upstream transport; a single
unrepresentable frame fails clearly, never drops an earlier accepted item.
Legacy receive supports text only and refuses structured pending items without
consuming them; clients choose one receive surface for the whole turn.

Typed events are TEXT, TOOL_BEGIN, TOOL_ARGUMENTS, TOOL_END, OPAQUE, USAGE and
FINISH. Tool indices are bounded request-local slots; stable nonempty call ID
and name are established once and checked on every later fragment. Argument
fragments cannot trigger execution. A successful TOOL_CALLS terminal follows
complete strict JSON argument assembly and wire DONE/EOF validation. Duplicate
IDs/JSON keys, invalid UTF-8/depth, conflicting terminal metadata, unknown finish
reasons and nonempty content after finish fail closed. Repeated identical finish
metadata and cumulative usage snapshots are accepted without double counting.

The OpenRouter descriptor is declared in `cog/openrouter.h`, with an explicit
borrowed net context and provider limits. It fixes Chat Completions, one choice,
strict yyjson pools and zero implicit retries/fallbacks. The provider package's
new public net composition header requires exported net usage requirements in
its delivery/build changes; core's forward declarations keep the graph acyclic.

## Durable messages and effect identity

A completed model turn produces one assistant message containing visible text,
identified tool calls, optional provider continuation and reported usage.
Streaming deltas are display/progress data, never separate durable messages.
Tool results correlate through `tool_call_id`, not names or array position.
The execution key is `(assistant node ID, call ID)` on a session branch.

Opaque continuation consists of namespace, pinned format, explicit byte length
and exact owned bytes. It is persisted/replayed separately from text, never
rendered or used to request hidden reasoning. OpenRouter uses namespace
`openrouter.chat-completions` and format `json-v1`; the private codec defines
its representation as a strict object containing only optional
`reasoning_details` array, with array entries preserved as uninterpreted JSON
values. It retains one final array across stream updates and reinserts that
assistant member beside role/content/tool_calls. Unknown required continuation
fields/formats fail UNSUPPORTED before tool execution or sending. Pass 2 requires a synthetic opaque round-trip fixture; the live research
had no such metadata. Usage availability is explicit; unknown cost/tokens never
become zero or guessed billing. Raw debug request/response fields may hold only
explicitly sanitized protocol data, never credential headers or secret-bearing
URLs.

Session constructors reserve node/payload/ledger capacity. Insertion deep-copies
the entire message atomically; reaching a limit never prunes existing history.
`copy_history` writes caller-owned arrays with transactional required-count
retry. Nested views remain session-owned; no history query allocates. Legacy
unidentified tool helpers remain inspection-only and cannot invent IDs.

An assistant call starts PROPOSED. A denial becomes DENIED. Before ALLOW can
start a tool, the application transitions to STARTED and successfully saves it
durably. STARTED can become SUCCEEDED, FAILED, CANCELLED or INDETERMINATE; final
states cannot start again. Results and final ledger state are saved together
before the next model request. Failed persistence never grants execution.
A crash after STARTED may occur before or after a side effect; load converts it
to INDETERMINATE, refuses ordinary replay, and never automatically runs it again.
This is at-most-once admission, not an exactly-once filesystem/process transaction.
Forking refuses incomplete/unresolved paths and never repeats ancestor effects.

The synchronous toolkit compatibility path also gets at-most-once semantics.
Its call constructor reserves a 64 KiB result capture. Each handler is invoked
once; output-capacity retries copy a cached result. Capture overflow may mean
an effect occurred and is terminal, never a cue to grow/reinvoke. Batch execution
reserves result resources before any effects, defaults to sequential, and retains
outcomes for every launched call, including failures. Blocking compatibility
calls are excluded from the responsive real-client loop.

Async descriptors have new/start/poll/cancel/result/destroy operations. An
execution constructor validates complete strict JSON and the documented schema
subset, resolves a static descriptor, and reserves all state without effects.
Unknown validation keywords fail UNSUPPORTED. Zero policy means UNDECIDED;
interactive prompting happens in the application between constructor and start,
while noninteractive ASK denies. Only explicit ALLOW plus durable intent starts.
Output copying is repeatable; truncation/possible effects/exit status are explicit.
Process tools own a process group, TERM/KILL deadlines, bounded pipe capture and
nonblocking reap. A handle cannot be freed while active; cancellation keeps
servicing until children are reaped. No arbitrary C callback or OS scheduling
hard-time guarantee is claimed.

Workspace tools use one opened root, descriptor-relative traversal and no-follow
checks for every path component; absolute/traversing/symlink paths are rejected.
They never change the process-global cwd. Read/search/edit/command inputs have
strict schemas and bounded work. Commands require both enablement and per-call
policy, use explicit copied environment and an owned process group. Permission
to run a shell command does not constrain what that command can do at OS level.

## Persistence version 2

The existing file format is version 1. v2 stores complete structured messages,
base64-encoded opaque bytes plus exact lengths/namespace/format, usage-known
bits, execution ledger and tree identity. Encoders/decoders enforce limits and
associations before publishing a session. Unknown versions and incapable custom
backends return UNSUPPORTED. All custom static backend tables need load_ex.

v1 text-only sessions migrate in memory and are rewritten only by explicit save.
v1 tool records lack call IDs; normal load refuses them, and explicit INSPECT
load exposes them with replay_safe=false. No synthetic IDs or guessed effects.
The load report records source version, migration, indeterminate tools and any
discarded tail. Truncated-tail recovery is opt-in and applies only to one
unterminated final log record after a validated committed prefix. Middle/bad
complete records, association corruption and unsupported metadata always fail.

Save writes a complete immutable generation, fsyncs its files, atomically
replaces one manifest and fsyncs the directory. It must not publish node and
index files independently. An IO failure around durability is treated as uncertain;
application execution does not begin on a failed STARTED save. Append uses the
same generation transaction for the new node and ledger; existing ledger updates
use save. Rebuilding a missing derived index is allowed from a validated log;
corrupt authoritative generations are not silently swapped or skipped.

## Transport admission and fair scheduling

The supported curl profile requires recorded build provenance
ENABLE_ARES=ON/ENABLE_THREADED_RESOLVER=OFF, runtime ASYNCHDNS, nonempty ares and
successful c-ares-only DNS_SERVERS support checked on a temporary handle then
reset. Ares presence alone permits hybrid threaded primary resolution; those
builds are rejected. The DNS PoC's c-ares-only cancel/cleanup evidence supports
this requirement, not a universal real-time promise. No QUICK_EXIT workaround,
abandoned resolver thread, implicit proxy or redirect fallback is allowed.
The initial package must reproduce the controlled DNS suite for its maintained,
security-patched shipped dependency build; PoC versions are evidence, not a
perpetual version/security claim.

Net request construction owns copied inputs and one transfer slot until explicit
request_release, even after terminal/cancel. service advances bounded work with
zero wait and returns an absolute deadline. peek/poll retain a stable response
view; ack consumes only the selected ID/sequence after downstream copying. A
full ring pauses reads transactionally. Cancel preserves retained bytes and
queues one terminal outcome; release explicitly discards them and frees handles.
HTTP status and transport failure are separate. No net retry/backoff occurs.

Core service hooks run before first dispatch, at due deadlines, before idle and
at least every configured event batch (default 16) even under sustained queued
work. IPC and stop checks receive the same fairness. Nonblocking hooks return
errors; only AGAIN is a normal no-progress result. Idle receives the minimum of
the next deadline and configured wait ceiling (default 10 ms). Legacy unbounded
wait_fn is incompatible with new service/idle hooks. Core handlers/hooks must
cooperate with bounded work; no scheduler can preempt an arbitrary blocking C
handler. Delivery Q must prove core/net/timer/IPC/cancel progress before K.

CLI configuration precedence is CLI > environment > config > defaults; secret
references resolve once at startup via env or an explicit host callback. Real
is the default; mock requires selection, and setup errors cannot switch modes.
The host initializes transport dependencies explicitly and supplies its net
context. CLI owns one bounded elapsed-time/attempt retry budget, default one
additional attempt, before accepted output/effects only. Deadlines, token/context,
round and output limits fail clearly. Incomplete/cancelled turns remain explicit
and cannot be transparently replayed.

## Compatibility and remaining gates

This pre-alpha contract revision intentionally requires source changes: provider
and store vtable initializers gain mandatory operations; endpoint/message/tool/
config structs gain fields; old positional aggregate initializers need updates;
net poll requires explicit service and ack; all real networking requires explicit
host/runtime and dynamic-memory acknowledgement. `struct cog_node_data`'s tag
becomes `struct cog_node_data_view` to compile with strict C++ shadow warnings;
`cog_node_data_t` and its accessor name remain. No binary compatibility is claimed.
Only the provider private include shim changes outside public headers/docs; no
production .c bodies or existing executable scenarios are changed in this pass.

Pass 2 must trace R01–R18 and scenarios 01–20 against exact calls, including
out-of-tree provider construction, retained overflow while polling, tool-result
resize after a file side effect, crash boundaries at every ledger save, opaque
round trip/fork, v1 unsafe inspection and v2 truncated-tail rules, two independent
net contexts, cancellation during actual DNS, and perpetually nonempty core
queues. It must resolve any API omissions before pass 3. Gate E independently
reviews allocation adoption, all lifetimes, ABI migration, error paths, include/
link closure and contracts. Production delivery packages remain scaffold tickets
until then; the coordinator refines them into implementation/review/integration
children after approval.

## Pass 1 validation

All 28 public headers (26 library and 2 CLI) compile standalone with GCC C11,
G++ C++17, Clang C11 and Clang++ C++17 under warning-as-error flags: Wall,
Wextra, Wpedantic, Wconversion, Wsign-conversion, Wshadow, Wundef, Wpointer-arith,
Wcast-align, Wwrite-strings, Wformat=2; C additionally uses Wmissing-prototypes
and Wstrict-prototypes, GCC C also Wlogical-op. This is 112 header compiles.
Local documentation links and diff whitespace pass. No live request, production
runtime test, sanitizer adoption claim or executable scenario completion is
part of this design-only pass; existing implementations must migrate to the
new mandatory contracts in the gated delivery packages.

## Pass 2 scenario amendments (345a536)

[The full pass-2 register and all 38 traces](REAL_CLIENT_SCENARIO_PASS2.md) amend
this initial pass. Their exact public-header contracts take precedence over
pass-1 shorthand above. D provides no production `.c` implementation.

REAL now defaults to a durable state-directory file store; explicit no-store
is text-only. CLI presence includes store mode/directory and an independent
base URL. Null-store save/append cannot satisfy durable admission. Owned-session
contexts require a current same-host execution guard, held across tools and all
other owned work; async execution constructors borrow it through destruction.
Legacy unguarded or synchronous paths cannot bypass this profile.

Shared transport consumers use request-local retained peek/ack. Typed queue
expansion has a bounded resumable cursor and an independent terminal slot; an
intrinsically oversized callback/item fails instead of permanently pausing.
Tool ID/name fragments are assembled before the one full BEGIN. Opaque stream
reconstruction is pinned in `openrouter.h`; blindly preserving separate streamed
text/summary fragments is insufficient. The synthetic pass-2 fixture specifies
complete exact bytes through persistence, fork and resend without displaying them.

Remote child updates now have public bounded apply/copy/ack/release and CGA1
encoding; cancellation still accepts the matching drained terminal acknowledgement.
IPC framing is uniformly nonblocking and constructor-bounded. Replay uses CGR1
DATA/GAP/CURSOR plus session/generation identity. A GAP during active streaming
uses the CGD1 atomically anchored full visible-display snapshot, which includes
incomplete text independently of ordinary replay-safe history.

Original17 is a quiescent same-host shared-store handoff, not live transfer.
Persistent owner tuple and exact checkpoint tokens, independent-open-description
OS guards, durable epoch compare/exchange, CGH1 correlation and advisory route
publication close its call contract. Source stays frozen after guard release;
target stages exact state, acquires the new owner guard, publishes initial route,
then starts work. Uncertain fsync/publication is INDETERMINATE, never assumed
rollback. Restart must fence/reap old owned processes and refuses unresolved
intents. Cross-host/live-handle migration is explicitly unsupported.

Q owns the nonblocking IPC profile needed by fair core scheduling before K.
G owns checkpoint/guard persistence; K/O own display snapshot/replay; N/O own
public child and same-host handoff integration. These assignments do not waive
E or permit implementation before the final header gate.

### G fork-point clarification

Fork returns the same immutable shared node rather than inventing new assistant
or effect identities. Nonleaf selection preserves the current active leaf until
`add_message(parent=fork_point)` creates/selects the divergent child; leaf
selection may select that leaf. Immediate snapshots always retain an active leaf.
This clarification preserves the existing public snapshot/restore invariant.
