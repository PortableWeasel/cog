# Stage 14.D: scenario pass 2 and amendment register

> [Stage 14.E pass 3](REAL_CLIENT_PASS3.md) records the latest recovery, bounds,
> compatibility and explicit DYNAMIC allocation decisions. Additional full
> independent final review remains required before E closes or implementation.

Ticket `345a536`; starts at integrated C `29b41e8`. This pass retraces **all 20
original scenarios and R01–R18**, with one sequence diagram and exact declaring
header table per scenario. It amends headers only; it does not supply production
C implementation, adopt a runtime backend, rerun paid requests, or claim any new
executable scenario passes. Pass 3 and independent gate E remain mandatory.

## How to read coverage

The original Markdown traces now replace their obsolete SVG call labels. SVGs
remain historical evidence. A table lists actual public calls, declared types,
enum members and fields, including constructor ownership and error/capacity
branches. Local application actions are explicitly identified as such; no private
record mutation, invented convenience function or future helper fills an API gap.

**Contract coverage** means a named public surface and its pre/postconditions can
express the flow. **Baseline executable evidence** means an existing test file
exists; it does not prove the new mandatory contracts. **Future conformance** is
assigned to a gated delivery package. The data-only synthetic fixture
[real-client-contracts.json](scenarios/fixtures/real-client-contracts.json) freezes
split tool identity, opaque reconstruction/base64 ownership, explicit config
presence and crash/result-resize oracles without claiming a running codec/store.

## Amendment register

| Gap | Trace trigger and prior failure | Header amendment / resolution | Delivery owner |
|---|---|---|---|
| D01 | REAL default with no `--store` could never meet durable STARTED, or silently bypass it | CLI real default durable XDG/HOME store; explicit disabled mode is text-only, unsolicited calls denied, no null-store execution | J/K/G |
| D02 | Sticky global net peek let adapter A select B's bytes indefinitely | `cog_net_peek_request`; id-local acknowledgement, independent stable views, bounded rotating provider service and minimum deadlines | F/H/Q |
| D03 | Full typed queue, too many events from one frame, or callback larger than total ring could stall forever/drop output | Reserved FINISH slot, bounded resumable frame cursor, terminal LIMIT for intrinsically unrepresentable input; transactional retained overflow | F/H |
| D04 | ID/name may be fragmented after arguments start; first fragment is not identity completion | OpenRouter privately assembles identity through validated turn, emits one full BEGIN then ordered argument/end events; no guessed ID/name redefinition | H |
| D05 | “Preserve final opaque array” omitted streaming reconstruction semantics | Bounded known text/summary reconstruction and discrete encrypted entries, conflict rejection, owned final bytes and exact save/load/fork/replay fixture | H/G |
| D06 | Output resize after a file effect could duplicate execution, or guard lifetime could end before cleanup | C's at-most-once cached result retained; D adds live owned-session guard borrow through async destruction, no synchronous bypass, explicit crash/save boundaries | I/G/K |
| D07 | Tail recovery could discard committed STARTED/result data under a generation manifest | Recover only bytes beyond validated committed count/checksum; never repair missing committed records or middle corruption | G |
| D08 | Null backend's successful no-op save looked durable; owned bootstrap/migration lacked precise identity | Null save/append UNSUPPORTED; immutable checkpoint token, durable owner tuple, explicit exact-generation load and guarded bootstrap/adoption | G |
| D09 | Presence/default matrix lacked independent base URL and explicit store policy | Separate base_url/endpoint_name, STORE_MODE/STORE_DIR/BASE_URL bits, exact config/env keys, explicit zero/ASK precedence and present-invalid rejection | J |
| D10 | Continuous queue or process lifecycle could starve cancellation; pending messages could allocate/drop on reenqueue | Constructor FIFO/peer/agent bounds; oldest retention on copy/enqueue overflow; nonblocking orchestrator service/admission; teardown before borrowed core/guard release | Q/N/O/K |
| D11 | Public status query could not ingest remote results; replacement status storage invalidated queued events | Peer/attempt/sequence-bound apply/copy/ack/release; bounded captured snapshots, portable CGA1 codec, explicit cancel-terminal race semantics | N/O/Q |
| D12 | Bounded replay lacked generations/GAP and active text resynchronization | CGR1 DATA/GAP/CURSOR, empty-window conventions; CGD1 atomic full visible snapshot including active/incomplete aggregate and exact session/cursor anchor | O/K |
| D13 | Polling a readable prefix did not make blocking full IPC receive/send fair | All sockets nonblocking, constructor frame queues and bounded service; partial cursors; recv overflow retention; transactional send/broadcast admission | Q/O |
| D14 | Original17 used nonexistent serialization helpers and unfenced routing updates | Quiescent same-host shared-store checkpoint + exclusive owner guard + durable epoch CAS; portable CGH1; target staged through initial route publication; restart/lost-ACK/IO rules; owned deletion/anchor replacement refused, store outlives every guard | G/N/O |

No known required public-call gap remains after these amendments. This is the
writer's pass-2 conclusion for independent review, not gate-E approval. Narrow
unsupported cases are explicit: active/live-handle or cross-host migration,
unknown required opaque formats, unavailable c-ares-only resolver profile,
nonzero third-party total-memory cap and ownership-incapable backend.

## Hard paths checked in the traces

- A tool effect occurs once, its result destination overflows, and a second copy
  retrieves retained output without another invocation. Failed final persistence
  never causes replay. Every crash boundary after durable STARTED is unresolved
  or INDETERMINATE until explicitly addressed; no exactly-once claim is made.
- An endless ordinary event queue still services provider/net/tools, deadlines,
  IPC partial frames, stop and cancellation. The count/idle bounds assume bounded
  cooperative handlers and do not promise OS or allocator hard real-time latency.
- Two adapters share one net context without consuming each other's retained
  bytes, and two independent net contexts can be freed independently under one
  explicitly host-owned dependency runtime.
- Out-of-tree providers embed the supported public first-member base and supply
  all eight callbacks. No private headers, dynamic registration or hidden global
  lifecycle/refcount is required. The c-ares-only admission profile is unchanged.
- Opaque continuation is never display text. Known streamed metadata is
  reconstructed privately and preserved as owned exact final bytes through v2
  persistence and fork. Unknown/ambiguous continuation fails before effects.
- A replay GAP during an active/incomplete turn uses the application display
  snapshot, **not** replay-only `copy_history`. Session/generation/sequence are
  validated together; cursor publication is transactional, including an empty
  replay window after restart.
- Same-host migration first freezes/drains/reaps source work and checkpoints under
  its exclusive guard. Source releases the guard but stays frozen. Target loads
  the exact immutable generation and remains staged. CAS publishes a new durable
  incarnation/epoch; target acquires its guard, publishes the initial advisory
  route, then starts work. No route or ACK grants execution by itself.
- CAS/guard use independently opened OS locks (including same-process contexts).
  Uncertain publish/fsync is INDETERMINATE. Lost ACK never rolls back an owner;
  abort/resume advances epoch. Restart must fence/reap old owned processes, consult
  durable state and refuse unresolved effects before a fresh owner can execute.

## Protocol evidence for opaque reconstruction

The existing live research disabled reasoning and proved no opaque continuation
case. This pass uses synthetic metadata only. The
[primary OpenRouter SDK accumulator](https://github.com/OpenRouterTeam/ai-sdk-provider/blob/main/src/chat/index.ts)
shows why consecutive text/summary deltas need reconstruction while encrypted
entries remain discrete. The header pins a strict supported reconstruction
profile; ambiguous fields/types fail rather than guessing. Its bytes remain
opaque to display/session code. This is a design inference from the primary
implementation, not a new live provider compatibility claim.

## Package ownership and gates

G owns v2 generations, migration, durable owner/checkpoint/guard and backend
capability enforcement. H owns retained typed JSON/SSE and opaque assembly.
I owns complete schema/policy admission, at-most-once result delivery and process
cleanup with guard borrows. J owns config/default-store/presence/credential setup.
K owns full-turn assembly, display snapshot state and codec, durable admission
and finite retry/round limits. Q owns fair core service **and the bounded
nonblocking IPC endpoint profile needed before K**, not just a scheduling hook.
N owns public same-host child/handoff protocol integration refinement; O owns
reconnect/replay and headless migration behavior. P owns installed link closure
and shipped dependency-profile verification. These are implementation obligations
against declared contracts; none permits unreviewed new public behavior.

E must independently review D's new guard profile, third-party dynamic allocation
adoption, callback/borrow lifetimes, local codec validation, supported JSON schema
subset, source/ABI migration and the full 38-scenario closure. If it finds a gap,
amend/retrace it before implementation. G's unowned v1→v2 save remains explicit;
owned adoption validates a safe exact v2 checkpoint, never silently rewrites v1.
Legacy unguarded writes cannot modify an owned session.

## Validation

Validation completed: **112** standalone object compilations (all 28 public
headers under GCC C11, G++ C++17, Clang C11 and Clang++ C++17) with warning-as-error
flags; **four** mixed-public-include external-provider consumer compilations.
The public base prefix is statically asserted and all eight callbacks form a
static const table without private headers. Identifier/member and local-link
audits pass for **38 diagrams, 876 exact table references and 188 struct fields**.
Fixture JSON/base64/length equality and scoped whitespace checks also pass. These checks establish declaration and
include consistency only. Existing production `.c` bodies still implement older
layouts/semantics and intentionally require delivery migration; a whole-product
runtime or sanitizer pass would not validate this design-only change.

No original or R scenario is labelled an executed release pass by this commit.
The existing 01–10 tests remain under `tests/scenarios`, 11–20 under
`applications/cog-cli/tests`; each trace names its baseline source and required
new executable assertions. No authenticated provider request was made in D.

## Scenario inventory

| ID | Individual trace | Contract state |
|---|---|---|
| 01 | [Scenario 01: User prompt → LLM → streaming → UI](scenarios/01-user-prompt.md) | Covered, independent review pending |
| 02 | [Scenario 02: Network stall → detection → recovery → resume](scenarios/02-network-recovery.md) | Covered, independent review pending |
| 03 | [Scenario 03: Subagent spawn → execute → status → result → parent](scenarios/03-subagent-lifecycle.md) | Covered, independent review pending |
| 04 | [Scenario 04: Cold start → init → first event](scenarios/04-cold-start.md) | Covered, independent review pending |
| 05 | [Scenario 05: Graceful shutdown with in-flight work](scenarios/05-graceful-shutdown.md) | Covered, independent review pending |
| 06 | [Scenario 06: Parallel tool calls with timeout and output overflow](scenarios/06-parallel-tools.md) | Covered, independent review pending |
| 07 | [Scenario 07: Auxiliary core crash → detection → safe recovery](scenarios/07-aux-crash.md) | Covered, independent review pending |
| 08 | [Scenario 08: Hot config reload with in-flight generation isolation](scenarios/08-config-reload.md) | Covered, independent review pending |
| 09 | [Scenario 09: Subagent-to-subagent messaging across cores](scenarios/09-subagent-messaging.md) | Covered, independent review pending |
| 10 | [Scenario 10: Fan-out/fan-in across auxiliary cores](scenarios/10-fan-out-fan-in.md) | Covered, independent review pending |
| 11 | [Scenario 11: UI disconnect → bounded buffer → reconnect → replay](scenarios/11-ui-reconnect.md) | Covered, independent review pending |
| 12 | [Scenario 12: Provider rate limit → bounded backoff → queue](scenarios/12-rate-limit.md) | Covered, independent review pending |
| 13 | [Scenario 13: Session persistence, recovery and v1→v2 migration](scenarios/13-session-persistence.md) | Covered, independent review pending |
| 14 | [Scenario 14: Conversation fork and branch isolation](scenarios/14-conversation-fork.md) | Covered, independent review pending |
| 15 | [Scenario 15: Multi-turn tool use with durable at-most-once admission](scenarios/15-multi-turn-tools.md) | Covered, independent review pending |
| 16 | [Scenario 16: Provider error propagation to user and durable state](scenarios/16-error-propagation.md) | Covered, independent review pending |
| 17 | [Scenario 17: Quiescent same-host migration with durable fencing](scenarios/17-session-migration.md) | Covered, independent review pending |
| 18 | [Scenario 18: Structured observability and trace collection](scenarios/18-observability.md) | Covered, independent review pending |
| 19 | [Scenario 19: Connection diagnostics and event stream](scenarios/19-connection-events.md) | Covered, independent review pending |
| 20 | [Scenario 20: User interrupt and queued input](scenarios/20-interrupt-and-queue.md) | Covered, independent review pending |
| R01 | [R01: Configuration, credential resolution and startup](scenarios/R01-config-startup.md) | Covered, independent review pending |
| R02 | [R02: Prompt through HTTPS, streaming display and complete durable turn](scenarios/R02-real-turn.md) | Covered, independent review pending |
| R03 | [R03: Fragmented SSE, UTF-8, tool identity and opaque data](scenarios/R03-fragmented-codec.md) | Covered, independent review pending |
| R04 | [R04: Complete tool call, durable intent, policy, effect and result](scenarios/R04-tool-round.md) | Covered, independent review pending |
| R05 | [R05: Denied policy, invalid schema and unsolicited tools](scenarios/R05-tool-denial.md) | Covered, independent review pending |
| R06 | [R06: 429, finite application backoff and exhaustion](scenarios/R06-retry-budget.md) | Covered, independent review pending |
| R07 | [R07: Accepted partial output followed by network loss](scenarios/R07-partial-failure.md) | Covered, independent review pending |
| R08 | [R08: Cancellation during DNS, connect, stream and backoff](scenarios/R08-cancel.md) | Covered, independent review pending |
| R09 | [R09: Command timeout/cancel, retained output and process-group reap](scenarios/R09-process-timeout.md) | Covered, independent review pending |
| R10 | [R10: Save, restart, version migration and safe recovery](scenarios/R10-persistence-migration.md) | Covered, independent review pending |
| R11 | [R11: Fork isolation, call identity and opaque replay](scenarios/R11-fork.md) | Covered, independent review pending |
| R12 | [R12: Configuration reload with pinned in-flight generations](scenarios/R12-reload.md) | Covered, independent review pending |
| R13 | [R13: UI detach, bounded replay gap and active-display resynchronization](scenarios/R13-ui-replay.md) | Covered, independent review pending |
| R14 | [R14: Bounded remote child attempt, cancellation, crash and result ingress](scenarios/R14-subagent.md) | Covered, independent review pending |
| R15 | [R15: Malformed, oversized or ambiguous provider data](scenarios/R15-malformed-limits.md) | Covered, independent review pending |
| R16 | [R16: Stop and shutdown with active transport, tools and saved intent](scenarios/R16-shutdown.md) | Covered, independent review pending |
| R17 | [R17: Context, token, turn, output and tool-round limits](scenarios/R17-limits.md) | Covered, independent review pending |
| R18 | [R18: Install, external provider, independent net contexts and cleanup](scenarios/R18-embedding.md) | Covered, independent review pending |


## Independent-review correction: owned deletion

Critic review found that legacy delete/delete_ could otherwise bypass the owner
lock by removing/recreating its anchor. The initial profile now returns
UNSUPPORTED for deletion of any owner-record session or ownership anchor,
regardless of require_owner and even after guard release. The check/deletion
serializes with bootstrap/adoption; no owned identity can be reset to absent.
`cog_store_free` explicitly requires all guard borrowers drained and guards
released. Original17 and R10 include this race/refusal and lifetime branch.
No new API or production implementation is added by this correction.
