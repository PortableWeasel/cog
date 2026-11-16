# N pass3 — whole consistency, edges and delivery audit

Starting full pass2 `8df27c4`. No implementation bodies, runtime substitutions,
paid requests or main merge. All20/R13/R14 were walked AGAIN against the combined
surface, then interface/error/allocation/ownership/dependency checks below. A
different final critic still must approve an exact coherent checkpoint.

## Pass3 amendments

| ID | Consistency finding | Exact disposition |
|---|---|---|
| A41 | remote worker cannot observe coordinator controller-role expiry | authorization linearizes at valid-lease coordinator forwarding; not-forwarded authorization expires, committed exact grant may finish; no distributed revocation fiction |
| A42 | control-only state/urgent cancel could wait on ordinary response lane | matching lane receipts for POLICY_STATE; UI/executor CANCEL independent; actual executor serial for prompt/branch, coordinator checks canonical anchor |
| A43 | public void app_free cannot return cleanup result to runtime | private checked execution_close consumes done/staged context, primary preserved; public wrapper contract unchanged |
| A44 | child SYSTEM serializer/source unit lacked exact input shape | child_context typed actual identities/members + bounded escaped JSON encoder, persisted before task/model; no authority from text |
| A45 | stale receipt wording confused its own envelope ID with answered update | inner wire_id equals outer.reply_to; own outer wire_id is independent, fixed UPDATE lane |
| A46 | residual child POLICY text exceeded chosen noninteractive role | PRIMARY-only policy target/CGV row; child ASK durable no-effect DENY, no UI wait |
| A47 | private states only prose, consuming cleanup scope unclear | explicit PUBLICATION/PREPARE states/events, named direct drain/close, original standalone values unchanged |

The policy forward is prepared/checked against a current canonical prompt or
branch before send; copied-send AGAIN accepts nothing and rechecks lease on
retry. OK retains one exact authorization under original identity, even if role
later expires. Executor rechecks actual prompt/branch serial/guard/deadline,
not an unrelated root replay sequence or nonexistent remote role state. No
replacement controller inherits that decision. Read-only old-lease outcome
lookup/ACK cannot admit a new mutation. No new revocation feature was added.

## Whole all20 final consistency walk

Every detailed diagram/coverage/error walk in
[pass2](REAL_CLIENT_HEADLESS_PASS2.md) was rechecked with pass3's authorization
and cleanup changes. This table records the final additional invariant checked;
all rows have no known declaration/ownership gap after current amendments.

| Scenario | Final pass3 check |
|---|---|
| 01 | primary user/save and text→publication split inside handlers, frozen pending arena, exact canonical commit before later ordinary effects |
| 02 | no retry after output/effect; shared checked monotonic clock, finite host eligibility and original deadlines |
| 03 | exact registry child tree/parent link, attach before begin, real SYSTEM provenance and task once, app cleanup before borrow return |
| 04 | no fork before runtime publication; quiescent unbegun close; initial durable empty/root view before UI discovery |
| 05 | core done ends driver once then independent bounded drain; native cleanup continues after primary error; close refuses live owner |
| 06 | K serial per executor, true concurrent independent child contexts, group absolute deadline/request credit bounds |
| 07 | immutable owned PID/kernel+managed auth, no foreign reap, ECHILD permanent loss; cached final frames/accepted terminal resolved before failure projection |
| 08 | exactly two logical R12 generations, explicit per-call coordinator resolver, active generation provenance, imported failed-attempt counter, consumed disconnect then secret retirement |
| 09 | coordinator mailbox source identity, full uint64 decimal IDs, per-attempt lanes, whole-message output proof/prefix cache, no repeated dequeue |
| 10 | closed group gate until complete registry reservation; prefix rollback owns leases, early remote cancel/retired watermark; input-order presence/result cap |
| 11 | authenticated generated-root discovery, stable server-issued lease, GAP snapshot/cursor distinction, bounded role/receipt retirement without duplicate admission |
| 12 | one application retry owner, no net replay, global worst-case credit never reset/refunded from unknown usage |
| 13 | G authoritative checkpoint/owner-only repair unchanged; no volatile registry reconstruction or child auto replay after parent crash |
| 14 | same-session safe fork/current incomplete branch, global view checked by coordinator and actual state serial checked by owner; old queued authorization cannot migrate |
| 15 | strict original schema/vtable references and role mask enforced at lookup/start; STARTED before effect; immutable result+terminal save before model/ACK |
| 16 | accepted actual partial text/result retained, explicit LIMIT/feed error rather than successful partial snapshot; primary error wins cleanup |
| 17 | no active child/input/view/future config; source exact save/guard release, target staged, exact+1 CAS, guarded READY, route then ACTIVATE receipt; abort only fresh explicit epoch |
| 18 | per-executor observer bridge beyond net callbacks; real pinned labels/time/usage, no bootstrap/task/body/credential logs |
| 19 | actual typed transport lifecycle, HTTP separate, retained request IDs forbid early reuse; consuming target detach preserved |
| 20 | owner signalfd, worker inherited mask reset, host bounded output unchanged; cancel independent lane and final-line known/unknown disposition |
| R13 | partial resync never renders/enables input; current primary policy nonce/branch identity; old forward-committed grant distinct from replacement controller |
| R14 | accepted CGA terminal wins, negative consume changes no semantic prefix, new immutable terminal envelope, startup/cancel race fenced; no old peer/epoch reattachment |

## Interface and error audit

- Public IPC/core/agent/orchestrator additions remain declarations with complete
  pointer/ownership/error/thread contracts; private host surface introduces no
  installed runtime API. CGW1 native management and CGL1 application magic are
  inside Q's actual opaque length-framed payload, no fictitious Q type field.
- Static dispatch only: existing states retain numbers; new PUBLICATION/PREPARE
  and private event IDs are declared before bodies. Providers/tools use original
  static const tables; role-selected table REFERENCE composition avoids copied
  workspace schema/vtable drift. Root/child masks are enforced, not just advertised.
- Constructors publish NULL on ordinary failure, copy strings under finite bounds
  and consume transferred resolved configurations exactly as documented. Output
  buffers have capacities; codecs validate full shape before capacity/write.
  OVERFLOW changes required output only; no unknown type/field fallback.
- Pure CGV validation scans the entire nested CGD1 and all later rows first,
  then checks all capacities, then writes.16 agents/1 primary policy maximum;
  status/outcome meaningful predicates explicit. No later bad row can leave a
  successful nested decoder's partial caller mutation.
- checked add/multiply applies to node/display/codec/frame/ring/client/child/
  secret reservations, group output escaping and request-credit arithmetic.
  uint64 counters/epoch+1 never wrap/reuse; mandatory cleanup identity/byte slots
  remain reserved. No codec casts uint64 IDs through lossy signed/double JSON.
- Driver failure freezes ordinary admission but not owned drain. Persistent
  unavailable clock/system cleanup cannot be represented as successful done:
  retain primary failure/ownership and retry bounded cleanup when usable; close
  never invents drain/reap/fencing. No synthetic time, blocking fallback or task
  restart makes cleanup appear complete.

## Constructor, borrow and secret audit

| Owner | Allocation/borrow rule | Ending rule |
|---|---|---|
| Stable runtime | fixed slots/commands/lanes/projection/config mirrors; named constructors only at admission | never-begun close or all-context drain/native terminal before checked close |
| Primary executor | real session/store guard/provider/current+future config; canonical pending copy separate actual drain buffers | checked consuming close, disconnect once on any return, secret afterward |
| Local child | exact registry session borrowed once; separate app/core/provider/net/observer/guard/config | destroy app/tool/context/guard borrows, then return exact session; record still one-shot |
| Remote child | owned guarded child tree; registry is status/identity shadow only | actual drain/factual durable ledger, terminal receipt then bounded attempt retirement |
| Staged target | exact loaded tree/config/preflight, no guard/save/input until committed stage | normal guard READY, route then ACTIVATE; failures frozen, no old-source thaw |
| Orchestrator | immutable native spawn PID, strong managed IDs, actual peers, fixed event/route reservations | exclusive exact reaping or explicit lost ownership; release record only no events/routes |
| Secret bootstrap | dedicated body/frame/TX/RX scratch, no generic event/receipt/UI cache | logical retirement/explicit abandonment wipe; preserve readable RX across EOF; generation zero on free |
| UI | server-issued nonreused logical lease, independent command/render cursors and finite caches | lease retirement makes old identity uniformly stale; no retarget/re-admit pending input |

No cross-thread app/orchestrator operation was introduced; only existing core/app
stop is cross-thread/signal-safe. Native children use no credential environment
or argv; host SIGCHLD/reaping/credential/directory lifetime profile stays explicit,
not silently enforced through global handlers or process-wide mutation.

## Source/dependency and verification audit

Actual [per-file tickets](REAL_CLIENT_HEADLESS_TICKETS.md) provide30 source tasks,
independent component review/integration/finalize and whole O package gate.
Every source and parent group depends on N; no source is dispatched by design
completion alone. Existing d5280b7 owns real orchestrator migration, not a duplicate.
O4 is one cohesive shared app lane with a private-layout/header stage BEFORE
bodies; pure wire/profile/projection modules can run disjointly after prerequisites.
No circular partial-runtime/stub integration is planned. Header/behavior gaps
return to architect; source-private implementation layout cannot invent authority.

Library dependencies remain acyclic: app→orchestrator→core/IPC; core never includes
application private headers or calls orchestrator implementation. CLI must export
the real new transitive dependency. Accepted R18 install/relative RPATH, assertion
execution, optimizer-safe fault oracles and M semantics stay intact. Actual O
oracles restore six M exclusions/all20/R13/R14 with offline TLS/tool/process work;
P retains all38/full supported-release gate. No paid request is required.

Writer verification for this pass:74 full-strict manual checks (40 new private
standalones,24 changed public standalones,8 combined/reversed repeated include
orders across GCC/Clang C11+G++/Clang++ C++11,2 C11 app_internal checks), plus the
maintained public critic-header-smoke target and header CTest under GCC and Clang.
SVG XML and rendered layout checked; all edited links/diff whitespace checked.
No implementation/runtime PASS is inferred. The different final header review
remains OPEN until it records an exact approved checkpoint.
