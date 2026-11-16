# O4 private implementation layout

Ticket860d5cc, approved N7eb24f1 with final pass3/A48, O base0aa7654. This stage
freezes shared implementation storage/helpers BEFORE bodies. It introduces no
public or behavioral contract, protocol, callback authority or implementation.
O4 bodies remain gated on ALL O1/O2/O3/O5/O6/O8 primitives, regardless of this
layout's approval. Existing source tickets and N docs stay authoritative.

## Pass1 — fewest shared records and file ownership

Three new headers keep one-way includes and explicit ownership:

- execution_internal.h: app-owned role/drive/authority/frozen publication state
  and cross-file helpers. app_internal.h adds only an opaque owned pointer; NULL
  preserves current standalone M layout use. No orchestrator include is forced
  into current app source build before its proper O4 CMake dependency is added.
- host_internal.h: constructor-sized nonsecret/secret arenas, full stream keys,
  command/client/attempt/mailbox/worker records. Every pointer has a fixed owner;
  no public event payload pointer/intent lease escapes handler return.
- composition_internal.h: stable coordinator and actual worker structs, group
  grants, toolkit-owned async handles/context, one handoff coordinator. These
  definitions are implementation storage, not authentication/admission APIs.

| O4 file/ticket | Sole body responsibilities / shared helper definitions |
|---|---|
| cli.c /8f94d02 | common resolved constructor cli_app_construct_resolved, checked resource disposal cli_app_dispose; public new/free wrappers; no independent session selection/re-resolution |
| execution.c /b89a8fc | declared N execution_* boundary, role/drive/authority axes, frozen view/commit/policy/version checks, cli_execution_barrier; checked close preconditions; no nested core loop |
| app_turn.c /e75a155 | original static K table plus PUBLICATION/PREPARE continuations, in-handler split before later effects, cli_turn_drain_one shared by run/manual; no second state machine |
| app_tools.c /c9b1548 | cli_tools_binding chooses original static table + fixed userdata, role/mask/schema/policy checks, PREPARE before guarded STARTED, cli_delegation_saved only after actual TOOL+terminal save |
| app_sessions.c /b5d2304 | cli_sessions_admit exact local borrow/remote new child/STAGED token; initial SYSTEM+guarded checkpoint before child USER; keep original L/empty-session rules |
| app_reload.c /0876b96 | existing R12 current/future ownership, authoritative explicit attempt/import count, resolved candidate publication/retirement; no startup precedence/callback reuse |
| host_runtime.c /725e4aa | stable core/UI ingress, cli_host_plan, cli_host_dispatch, immutable lane/receipt/role records, global reserve/start/release, registry intents/placement/fair service/results/mailboxes; no worker socket reader |
| delegation.c /1110ab9 | static original descriptor table + strict schemas, explicit async new_/start/poll/result/cancel/destroy, installed context lifetime, local/worker adapter tag, cli_delegation_saved |
| worker_host.c /8927216 | sole worker parent endpoint reader, native bootstrap/heartbeat/control routing, actual executor slots, first-admission/watermark/cancel latch, cli_worker_delegation_step |
| handoff.c /4b68739 | existing CGH/G coordinator steps and exact tuple/token/phase ACK; catalog borrow independent of app/store replacement; no source thaw |
| main.c /27508d8 | O6 role parser, existing bounded host output/signal lifecycle, published host begin/step/close and installed worker startup; no secret argv/env or app private-field shortcut |

app_display.c/public CGD1 remains complete and unchanged. O5 codecs and O8
projection/client use declared N types rather than these implementation structs.
Only the coordinated O4 lane edits app_internal.h or shared production/test
CMake. After freeze, per-file leaves can use the named helpers but cannot mutate
another file's owned subrecord or invent a new authority transition.

### Constructor allocation and borrow table

| Owner/reservation | Fixed bound / allocation time | Release/borrow ending |
|---|---|---|
| runtime clients/commands/groups/slots/streams | configured finite clients<=16, workers<=8, children<=16, executors<=18, commands<=1024; groups<=children; new before publication/fork | no outstanding sends/effects/borrows before checked close |
| each nonsecret stream | separate inbound/outbound FULL key; pending+original/final bytes at construction; per-lane schema-specific cap | retain exact bytes through final consumption/successor proof, then scalar retired watermark |
| PROFILE secret exchanges | one startup per worker plus one per executor; fixed by-value original probe/session+owner+checkpoint in EACH exchange, dedicated body/frame/scratch; pinned generation owner remains live | original probe remains immutable through pending/ACK/live-generation duplicate proof; retire exchange/borrow then clear metadata, full-capacity volatile secret wipe, not generic receipt cache |
| UI send/resync/projection | full committed replacement frame per admitted client plus independent projection scratch; ring configured separately | copied transfer immutable through output; no executor lifetime borrow |
| command and forwarded operation | independent copied originals/results and COMMAND/CANCEL slots | client receipt retirement cannot free active executor operation bytes |
| group/tasks/output/status | whole count/credit and128+1024*count+6*payload proof before STARTED; named app/tool constructors bounded afterward | prefix gate remains closed on failure; actual members stay charged until cleanup/result save |
| authoritative mailboxes | per child fixed entry/text bounds, whole-item receive fit, retained whole prefix reply | copy/save retry never dequeues twice; no growing queue/fragmented message |
| executor apps/config/net/observer | named constructors at admitted creation, one net+bridge per executor; no runtime table growth | app/tool/provider/config/guard ends; net ends; observer ends; return local registry borrow last |
| app frozen view | separate full item/text arenas reserved with app, source mutable drain buffers never alias | exact canonical commit before ordinary continuation; later dirty drain state needs another capture |
| registry/control core | separate owned control session, real agent records, borrowed actual local child tree | all started children drain and borrows return before fatal core_free lease disposal; never restart core |
| shared catalog | independent require_owner store, no executor-store alias | outlives orchestrator+handoff+guards; route names never grant ownership |
| current/future mirrors | opaque resolved copies of at most two logical IDs; source configuration callbacks not retained | children pin exact generation until provider disconnect; no third live provider/logical generation |
| staged source/target | source exact token/expected tuple, target exact token/next tuple, separate active flags | actual release→CAS→acquire→route→ACTIVATE; failures freeze, no epoch rollback |

cli_host_plan is pure checked arithmetic: capacity/byte products, full CGL/CGV
framing overhead, two directions and maximum live keys, independent final/control
reserves and JSON worst-case escapes. No blind max-frame multiplier for every
lane. Impossible full snapshots/group output/secret/profile combos return LIMIT
before publication/STARTED, never successful truncation. Named admission
constructors remain explicit allocation boundaries; poll/service never grows any
array, retries a consumed provider or runs a blocking rollback/drain loop.

Configuration remains opaque: only approved options/labels/tool-mask getter and
startup finish_host are used. finish_host follows actual host canonical-directory
verification and precedes bind; clone/bootstrap/reload never call it or rerun
sources. CGB transport decode supplies no host-init/dependency/observer authority.
A tool capability mask is not ALLOW and is rechecked at actual STARTED admission.

Pass1 verification:18 strict manual include checks:4 standalone+combined C11
per GCC/Clang,3 standalone+combined C++11 per G++/Clang++, repeated/reversed
orders. Existing app_internal C11 atomics remain C-only; no C++ promise added.
No source body/test or runtime conformance claim. Pass2 now traces actual
retention/cancel/grant/publication/handoff/cleanup edges and amends layout only.

## Pass2 — targeted state/ownership walks and amendments

1. **03/10 pre-STARTED reservation and prefix failure:** toolkit new_ copies
   strict arguments and reserves capture/schema pools. app_tools PREPARE binds
   real owner/node/ordinal/full call ID/pinned generation/tasks/deadline, calls
   delegation_prepare. Local goes to cli_runtime_reserve; remote retains exact
   RESERVE/grant wire. No grant means no durable STARTED. Successful guarded
   STARTED token reaches cli_runtime_start once. Group gate remains false while
   admitted prefix records/app constructors exist. Failure sets ROLLBACK, cancels
   and disposes queued leases incrementally; actual/absent members remain
   distinct. Started groups never refund unknown credit. Read-only native health
   plus separate reserved_slots supports fair local/worker placement; no fake
   heartbeat progress. Group/result cannot be recycled until child cleanup,
   borrow return, pending leases/updates and actual parent result save allow it.
2. **R14 early CANCEL and retained terminal:** slot lookup matches full active
   attempt before high watermark. Initial worker profile ACK precedes attempt
   frames. Added first_admission_sent per slot: send-OK, not local intent/AGAIN.
   Host picks ascending first admissions per worker; an unsent cancelled attempt
   can end locally only if this flag is false. Early CANCEL slot latches before
   child profile/start. Late frames get exact ABORTED, no task admission; retained
   terminal receipt plus actual cleanup precede scalar watermark retirement.
   Factual child ledger/status and parent's accepted prefix stay distinct;
   CANCEL_CONSUMED frees only matching wire, then drained terminal uses a new
   wire in the same unaccepted semantic slot. Original final receipt/original
   bytes remain through loss; after reclaim no fabricated conflict comparison.
3. **09 mailbox pressure:** delegated SEND/RECEIVE adapters do not manipulate
   coordinator-owned mailbox fields directly. Added cli_runtime_mailbox_step
   owner helper and explicit message view into handle-owned arguments. All
   frames/responses use per-attempt MAILBOX lane; no blocked sibling/global
   request slot. Full accepts none; whole-item fit checked before copy. Reply
   prefix is immutable in its own capture; output copy/save retries never
   dequeue again. Worker relays only; no invented independent mailbox.
4. **05/07 fatal/drain:** core drive_end occurs once, then cli_turn_drain_one
   advances actual provider/tool/unstarted cleanup without core_step/restart.
   Host/native stop continues after first error. Checked close refuses live work,
   then destroys handles/context before app/store/guard. Local registry borrow
   stays live until app consumption and is returned before agent release or
   fatal coordinator core_free. Fatal core queued intent leases are disposed by
   existing destructor only after all separate started executors/borrows drain.
   Worker PID reap alone cannot certify descendants stopped; uncertainty stays
   durable recovery-only. No raw event/arena pointer survives dispatch.
5. **08/R12 pin and failed-attempt handoff:** current/future mirrors name exactly
   two logical generations. Child clone/bootstrap pins active G even with a
   published future. Getter mask is copied, not inferred from policy. Global
   reload_attempts_used includes failures/skips; source view, BOOTSTRAP and staged
   admission retain it. A new target's tree cannot replenish64 tries. Consuming
   provider disconnect returns before matching config/secret retirement; no
   extra provider/mirror callback authority or automatic source resolution.
6. **11/R13 publication and command lease loss:** in-handler completed user/text/
   assistant/tool mutations call barrier before next ordinary effect, including
   direct handler subphases. Existing static PUBLICATION row waits exact canonical
   commit; it never polls stdin/direct FILE output. Drain during an outstanding
   copy sets newer_view_dirty, preserves bytes and continuation, and later copies
   actual new state. Commands retain canonical anchor and executor serial
   separately. Forwarded op copies outlive UI lease/receipt retirement; CANCEL
   owns an independent slot. Pre-forward send AGAIN rechecks lease, send OK
   commits that exact authorization, not replacement controller approval.
7. **17 staged authority:** source freeze retains guard/token; target constructor
   does exact-token load/preflight only. Actual PREPARED permits release; actual
   exact owner CAS permits target normal guard; actual READY permits route;
   exact ACTIVATE/ACK opens fresh-input admission. Driver and authority axes
   prevent accidentally beginning staged or restarting stopped source. Catalog
   remains independently owned beyond source app disposal. Abort freezes without
   old-epoch rollback. Added owned canonical directory copies/FD pins to make
   finish_host's actual host identity/stability precondition concrete.
8. **Secret ingress/unwind:** trace found generic worker ingress/decode arenas
   could receive credential-bearing CGB1 before kind discrimination. Replaced
   them with secret-capable full-frame/decode arenas; only validated nonsecret
   traffic can be copied to generic records. Wipe complete reservations on every
   retirement/failure, including partial new unwind; never copy into event/receipt
   originals or UI. Native HELLO/heartbeat/shutdown-ACK now have three independent
   fixed control reservations so ordinary queues cannot consume shutdown capacity.

No public/behavior gap or new protocol was introduced by these layout amendments.
N's source/target, exact native binding and per-call resolver authority remain
unchanged. Repeated strict include checks pass18/18 after the amendments. This
is declaration/ownership validation, not runtime tests or O4 body authorization.

## Pass3 — include, ownership and static dispatch consistency

Final consistency walk found three implementation-only joins and pinned them:

- UI stream dedup must survive a fresh transport peer under the SAME retained
  logical lease. cli_stream_key.owner_peer is the immutable WORKER attempt peer
  only (0 UI); client_record.peer/proof holds current authenticated UI transport.
  Treating that transport as part of the logical command key could admit an old
  request twice after reconnect. Worker replacement/reconnect still has no rebind
  authority. Active attempts/leases are looked up before scalar retired counters;
  retired unknown identities remain uniformly stale, not newly allocated records.
- A provisional RETRY may precede a final receipt without permitting the worker
  to retire its item. Lane layout now has separate provisional reply bytes/wire/
  prefix from cached immutable final reply+answered original. Identical RETRY
  retransmits same bytes/ID. The initial RUNNING+terminal profile bounds semantic
  transitions; before each ordinary admission reserve enough final/control wire
  IDs/bytes, including changed-prefix provisional reply and negative-consume
  resolution. Supersession never rewrites a queued/retained envelope or grows a
  cache. Final accepted prefix is distinct from wire/nonce/replay/store counters.
- Provider preflight schemas precede context installation. cli_tools_binding
  permits schema-only lookup with user=NULL, filtering role/mask against original
  descriptors without tool callbacks. Actual userdata lookup refuses before
  attachment and enforces identical role/mask. Global DENY/ASK and actual guard
  remain separate effect checks; schema presence is never permission. Shared
  resolved construction marks local borrowed-session ownership before installing
  its pointer, so even early failure cannot run the standalone session destructor.

Include graph: no library/O5/O6/O8 header includes these layouts. app_internal
only forward-declares its owned pointer; the three new implementation headers
are independently C11/C++11-compatible and do not include app_internal. No
configuration definition, provider/toolkit private vtable or vendor parser type
crosses this boundary. All numeric maxima are the existing N limits; capacities
are explicit, checked at named constructors and never enlarged by service.

Static dispatch audit: app_turn owns its existing static const table/transitions
and adds PUBLICATION/PREPARE/common cancellation/config-notice rows in every
eligible state. Saved continuation IDs select original handler phases after
canonical publication, never a runtime callback table. host_runtime owns ONE
static control-state table with exact protected IPC and copied agent-intent
handlers; worker_host owns its ONE parent reader/static kind switch. Common
lane records hold identities/bytes, not function pointers or a generic RPC
registry. Original workspace/delegation table references remain static const;
no flattened descriptor/schema copies, registration or nested run.

Failure/unwind review (all first errors retained): constructors initialize every
owned pointer NULL and fd -1; root_options/resolver inputs never retained; UI
endpoint remains constructor-owned until successful core transfer; canonical
path/FD pins stay until all borrowers end. PROFILE receive/decode/body/frame/
scratch/TX/RX capacities wipe completely on every abandonment/failure/free,
never ordinary metadata caches. App handle/context destruction precedes guard
release; provider disconnect consumes/detaches once on ANY return, then secret
clear; net ends before observer; local session borrow returned after app close.
Guard-release refusal retains source ownership; successful release never thaws.
Catalog outlives handoff+orchestrator and app replacement. No close returns done
with live provider/process/guard/registry borrow, no consumed-pointer retry or
whole-runtime leak substitute. The owning body must preserve these invariants
on every partial constructor stage before a pointer is published.

Rechecked03/05/07/08/09/10/11/14/17/20 and R13/R14 against final layouts/helpers:
no known private call/ownership gap in this targeted layout stage. This is NOT
another N whole-design gate, O runtime/scenario PASS or body dispatch permission.
Different final critic must review the exact frozen commit; root integrates only
approved headers/docs. Repeated standalone/combined/reversed strict GCC/Clang C11
and G++/Clang++ C++11 checks pass18/18 after all changes; no source body or runtime
fixture was created. All11 source owners retain their existing N contracts and
O prerequisites; private-file parallel dispatch starts only after layout freeze
and primitive completion, with shared app_internal/CMake edits coordinated.


## Cycle1 amendment08d1b54 — original bootstrap probe retention

Different final review found one missing implementation storage field: a duplicate
CGB1 must re-encode the ORIGINAL probe after dedicated ingress/frame scratch was
wiped, while worker-global probe state may already name another child, reload or
handoff. `cli_secret_exchange.probe` now owns one complete
`cog_cli_bootstrap_probe_t` by value. Its session is already a fixed128-byte array,
owner/checkpoint are values; assignment/deep copy creates no string borrow or new
allocation. The exchange's source pointer continues to borrow the exact retained
resolved generation, and the host transport template remains immutable as already
required. This is storage for existing N duplicate rules, no new authority/API.

Targeted declaration pass1 (capacity and initial ownership): at each named
constructor reserve exchange metadata with checked count*sizeof(exchange), now
including the probe. Coordinator bounds stay workers<=8 plus executors<=18;
each worker has its startup exchange plus<=18 executor exchanges. Secret body/
frame/scratch byte reservations and counts do not grow. Validate/copy original
session/current-owner/immutable-checkpoint together with original key, purpose,
logical reload count, generation and source before exposing pending/ACK state.
Worker/global `probe` may describe its current admission context but is NEVER
substitute duplicate-proof storage. No heap session copy/pointer into receive
scratch is permitted by this by-value field.

Targeted pass2 (duplicate and concurrent-generation trace): exchangeA captures
sessionS/ownerE/token5 under G1, then wipes the full temporary secret ingress.
Normal saves may advance S to token6; exchangeB for a child/reload/handoff may
replace the worker-global current probe with a different tuple. A duplicate of A
is re-encoded only from A.source + A.probe + the fixed transport and its exact
bound purpose/count/key. It compares original bytes and replays its existing ACK;
it never reads latest G, copies B's probe, reruns a resolver or saves/adopts a
session. Parent and worker each retain their own original value when they own
that exchange. Receiving/copying a new profile does not mutate an admitted A.
Changing any original probe identity/bytes is a conflicting duplicate, not a
request to refresh it. Existing finite lane/watermark/error rules apply.

Targeted pass3 (retirement, failure and lifetime): keep the probe immutable while
pending, ACK-retained or while the live source generation supports duplicate
comparison. Retire the exchange according to the existing bounded stale-watermark
rules BEFORE ending its exact source-generation borrow; R12 may not free the
borrowed generation underneath duplicate re-encoding. Once retired, stale traffic
cannot resurrect its cleared source/probe or receive a new latest probe. Constructor
unwind/abandonment clears metadata and wipes ENTIRE secret arenas; ordinary close
retires exchange first, then releases its explicit generation pin. No raw secret
original cache, extra logical generation/provider, store reread, runtime allocation
or mutable global is introduced. Header-only strict checks below do not claim
runtime duplicate conformance or permit O4 bodies ahead of their prerequisites.

Amendment verification:18/18 full strict standalone/combined/reversed checks on
GCC14/Clang19 C11 and G++14/Clang++19 C++11 pass, including a compile-time128-byte
owned-session-array assertion. Only host_internal.h and this document changed;
no .c/shared CMake/configuration/O6 edit. Different independent re-review remains
required before the layout gate closes.
