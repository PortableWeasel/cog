# N header iteration evidence and scenario map

Design035219b, O0e35c23, baseline M dacbd199. All changes are declarations/docs/
diagrams, no implementation. Incremental reviewer feedback is input to pass1,
NOT a replacement for three whole passes or different final review. Runtime
claims remain exactly M's six orchestrator exclusions until actual O delivery.

## Pass1 — assembled surface

Goals/blocks/cross-check are in HEADLESS_CONTRACTS; the
[system diagram](headless-integration.svg) shows process/fabric/authority owners. Public foundation
ipc/event/core/agent/orchestrator headers plus private host_runtime/execution/
control/child_profile/projection/delegation/worker_host/host_client/host_options/
handoff declare every new intermodule boundary. Existing G/CGA1/CGR1/CGD1/Q/J/K/L/
R12/observation/host-output/empty-session contracts are consumed, not replaced.
Pure CGW1 native management and CGL1 private app framing remain distinct.

Research used actual M source and config_writer's read-only a2e45c0 inventory,
existing standalone IPC/process PoCs and shipped public primitives. No new
production or synthetic mock implementation was written to demonstrate design.
The exact native SO_PEERCRED/exclusive-reap profile was traced against current
orchestrator's flawed hello-PID/first-connector behavior, then explicitly fixed
at contract level. Supported Linux host, stable configured directory identities,
exclusive child reaping, no child delegation recursion or worker reattachment,
no cross-host/live migration are deliberate finite release boundaries.

### Incremental amendment register

| ID | Found by exact trace | Contract resolution (all pending full passes) |
|---|---|---|
| A01 | bounded core yield/UINT64_MAX/stop could sleep despite work | step info immediate eligibility + finite max_wait, shared engine/clock |
| A02 | queued final frame loses live peer query after EOF | protected kernel proof copied through core lease; retained role proof |
| A03 | spawn returns before actual peer assigned | strong managed incarnation/number vs actual fabric peer |
| A04 | auto/external reaper can recycle immutable PID | validated stable SIGCHLD/exclusive exact reaping, WNOHANG before signal,ECHILD fences forever |
| A05 | final control event IDs exhausted by ordinary traffic | reserve both three transition slots AND identities before fork/capture |
| A06 | secure wipe could erase readable retained RX at socket EOF | logical retirement/explicit abandonment wipe, committed complete RX preserved |
| A07 | cancel rejects n, naive transport ACK makes CGA gap/deadlock | independent reviewed receipt payload + exact wire proof + new envelope in unaccepted slot |
| A08 | worker reconnect changes immutable owner_peer | unsupported; freeze/drain/fence, UI reconnect separate |
| A09 | registry child tree differs from new app tree | exact local borrow; stable coordinator explicit parent ID; remote shadow never fake history |
| A10 | unbegun shutdown had no close path | direct quiescent close, no begin/fork/input just to destroy |
| A11 | app display counter differs from replay counter | coordinator CGV1 commit plus private PUBLICATION before later ordinary effects |
| A12 | pending view overwritten by drain; one dispatch still runs send/start inline | dedicated immutable copy+dirty marker; split internal handler phases |
| A13 | unknown root ID prevents first attach discovery | HELLO-only runtime0/empty session; authenticated response binds root; reconnect refuses retarget |
| A14 | GAP empty payload cannot carry full view | explicit RESYNC_VIEW after GAP, exact committed anchor, CURSOR after checked render |
| A15 | controller disconnect can strand role | finite role reconnect lease; accepted receipts independent (pass2 pins all EOF) |
| A16 | remote primary input/policy lacked typed directional path | EXECUTOR_COMMAND/result,POLICY_STATE,copy_policy/apply_command,nonce+serial; child ASK explicitly noninteractive DENY |
| A17 | remote G1 active/G2 published delegation ambiguous | actual active/published facts and explicit pinned generation in reserve/start |
| A18 | remote constructor cannot reserve global credit before STARTED | nonblocking PREPARE + reserve/grant before STARTED, then exact start reservation |
| A19 | prefix spawn OOM cannot atomically erase records/fabricate missing IDs | nonexecuting group gate, normal rollback cleanup, member presence/aggregate failure |
| A20 | one whole-worker pending frame blocks sibling mailbox poll | per-attempt/session lane keys, independent control/update/mailbox reservations |
| A21 | child has no way to learn sibling IDs | bounded host SYSTEM context with actual canonical decimal group IDs; unchanged task follows |
| A22 | private config reread/secret cache/active probe race | resolved clone; dedicated outbound bootstrap wiping; exact immutable INSPECT probe separate current owner; no latest-token comparison |
| A23 | worker native HELLO/heartbeat bytes undeclared | public CGW1 literal codecs, no app-header library dependency |
| A24 | QUIESCE token unknown/new phases and staged constructor absent | checkpoint0→CHECKPOINTED→PREPARED/release/CAS→guarded READY→route→ACTIVATE |
| A25 | abort/source old-guard thaw contradicts stronger G | no thaw, exact checked+1 epoch; shutdown then explicit L RESUME/RECOVER reclaim |
| A26 | lost ACTIVATE reply could repeat authority/work | retained exact activation receipt, no fresh input forward until ACK |
| A27 | nested CGD1 decoder could partially write before bad CGV rows | nonwriting full scan then capacity check and write pass, fixed16 agents/1 primary prompt |
| A28 | private C++ struct tag hides same-named function | renamed step-info tag; full Wshadow preserved |

### All20 + real-client call map

This is pass1 coverage inventory, not a full pass2 or runtime PASS. Each row names
the real composition and exact declaration owner. Full sequential trace/error
walks follow in pass2; original historical substitutes are not accepted evidence.

| Flow | Exact new/reused header path | Required actual O oracle |
|---|---|---|
| 01 prompt/stream/UI | host_client_step/control command→execution_apply_command→execution_step/copy_view→projection_commit→CGR1→projection_decode/CURSOR | real accepted stream, full exact anchor, no repeated prompt |
| 02 stall/recovery | execution_step + existing provider_poll/cancel, net service, K timer retry, projection retained view | actual offline TLS stall, no retry after output/effect |
| 03 child | delegation tools/PREPARE→agent_spawn/copy_config→session_borrow→execution_new/drive→agent_apply/copy/ack→parent TOOL save | actual local child prompt/tool/result/cancel on exact registry tree |
| 04 cold start | host_options_open→host_runtime_new→execution_new(ROOT initial checkpoint)→begin/step,CGW1 readiness | constructor no fork on failure; explicit no-begin close; real empty restart |
| 05 shutdown | runtime_request_shutdown/step/close,worker_host_step/close,orchestrator request_shutdown/service | live net/tool/children drain and exact reap, no blocked output/global mutation |
| 06 parallel tools | existing I execute_all(component), independent child execution_step slices, finite group budget | actual independent effects overlap, timeout/drain all, no cached-handle fake overlap |
| 07 aux crash | orchestrator_spawn/service/copy_event/ack_event + worker identity,CGA cancel/failed ingress | actual owned worker death→identified attempt uncertainty, no effect replay |
| 08 reload | app_reload or execution_reload_resolved,child profile clone,copy_configuration | old turn/child generation remains pinned through parent publication and retirement |
| 09 sibling messaging | delegation send/receive + CHILD_MESSAGE/MAILBOX_POLL through coordinator-owned mailbox | actual child model tools route exact source/group, duplicate/no-second-dequeue |
| 10 fan-out/in | delegation_prepare + group-gated agent_spawn/local/remote drive + retained result ACK | real simultaneous local/worker child executions, input-order results, cancel drains |
| 11 UI detach/replay | projection snapshot/copy_after+CGV1,CGR1; host_client step; typed command cache | socket detach during stream/policy, GAP resync, no repeat effect/approval |
| 12 rate limit | provider error/cancel + K app budget/backoff, group request-credit ceiling | real offline429, max one admitted retry, no hidden net retry |
| 13 persistence | existing NEW/RESUME/RECOVER G guard/session helpers, execution ROOT admission | exact checkpoint/empty reservation restart; crashed STARTED needs explicit recovery |
| 14 branch isolation | typed FORK/execution_apply_command + existing L/session fork; CONTINUE exact incomplete leaf | same session safe ancestor, preserve sibling/opaque, no implicit queued fork |
| 15 tool loop | static table references workspace+delegation + I strict schema; K STARTED/result save | actual tool model continuation, no fake model/tool completion |
| 16 provider errors | execution_step/drain + retained primary/obs error, projection failure barrier | partial text durable/displayed or explicit failure, no unsafe retry/new input |
| 17 migration | handoff_request/step→execution_quiesce/STAGED/release/stage_guard/activate + G/CGH1/route | actual two guarded executors, once-only effects, failure at every authority boundary |
| 18 observation | existing per-executor observer/net scope, exact pinned cfg IDs, owner drains | actual provider/tool spans, child/root labels, no secret/body/task logging |
| 19 diagnostics | existing typed actual transport feed + per-executor bridges | actual TLS/connect/stall/cancel/HTTP classes, no fabricated diagnostic events |
| 20 interrupt/queue | signalfd owner request_shutdown/command CANCEL; K queued-fresh distinction | actual active cancel then queued/new input, incomplete state preserved |
| R13 compound | 01+11 plus pending ASK/incomplete, tiny decode/send, controller lease/CURSOR | exact no-repeat/gap/cross-session/slow peer oracles |
| R14 compound | 03+07+09+10 plus receipt cancel race/old epoch/parent death | immutable accepted terminal, negative wire consume, actual effect uncertainty |

### Pass2/Pass3 gate status

Pass2 full sequential all20+R13/R14 traversal is recorded in
[the pass2 retrace](REAL_CLIENT_HEADLESS_PASS2.md), including A29–A40 amendments.
Pass3 full consistency/
NULL/overflow/clock/OOM/lifetime/dependency/self-containment review is PENDING.
Then one more DIFFERENT final critic must approve exact coherent checkpoint.
No O implementation ticket is ready merely because a row exists here.
