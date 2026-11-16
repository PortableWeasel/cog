# Private control and bootstrap contract (N pass1)

`src/control.h` pins application-private CGL1 envelopes. No new general RPC
framework, mutable registry, socket authentication by HELLO text or library
dependency on application headers. The static kind/role table owns every ingress
case. Normal standalone M host output and public app APIs remain compatible.

## Roles, identities and bounded command retirement

The UI and worker listener have distinct fixed paths/owners. UI admits only
kernel-credential same-effective-UID clients through protected Q ingress. The
first explicitly requesting controller claims one bounded logical client record;
other clients watch. A controller can reattach its own retained incarnation while
disconnected; no other simultaneous transport can take it over. Same-UID peers
are in this local trust boundary, not protection from a malicious same-UID process.
No attached client can select workspace/store/provider implementation/credentials
or raise child permissions/resources. Worker commands require the actual managed
spawn/kernel PID+UID/HELLO identity binding; UI peers cannot send worker kinds.

Each sender stream has nonzero monotonic outer wire IDs. Replies identify the
exact answered wire separately from CGA semantic prefix, CGR cursor and G owner.
One accepted UI command owns its copied original bytes, admission/result and
receipt until COMMAND_ACK; retransmit exact bytes under the SAME identity only.
ACK reclaims into a retired watermark; covered old IDs are uniformly stale and
cannot re-execute. Full command reservations reject new admission before copying
effects; duplicates remain answerable. Accepting a command does not mean its
later execution succeeded. A new coordinator incarnation cannot restore volatile
dedup from a client's claim: full resync, old pending command marked uncertain,
no automatic retransmission as a new command. Disconnect never undoes admission.

UI_HELLO alone permits runtime_incarnation0 and EMPTY session_id(length0).
--attach/--watch know only socket and never guess IDs or read store/config. The
same-UID authenticated server replies with its actual root session/runtime in
COMMAND_RESULT header before any session-bound command; client rejects other
traffic until this discovery is complete. A reconnecting already-bound client
refuses another root session, never retargets its pending command/view to it.
It carries version1 u8, role u8
(controller1/watch2), replay generation u64, replay after_sequence u64. Header
sender_incarnation is the fresh nonzero client incarnation; a new client chooses
it with checked OS entropy. Matching retained identity/controller reservation is
checked before replacing its disconnected link. Server replies COMMAND_RESULT
with its runtime and canonical snapshot/replay state. Wrong generation receives
GAP/full resync rather than executing input to rebuild display.

All other envelopes require the exact discovered runtime. UI/local traffic has
worker/owner/agent/attempt0. Worker frames carry the exact managed identity. Agent
traffic additionally carries immutable actual owner_peer/agent/attempt from
already admitted records; decode never allocates authority from those numbers.
Final accepted frames preceding disconnect retain their immutable proof through
event leases. No fresh connection rebinds a remote attempt; worker disconnect
freezes/drains/fences it. UI reconnect is a separate fabric/lifetime.

## Literal nested payloads

All integers below are big-endian with no native struct/padding/trailing bytes.
Strings are length u32 then exactly that many non-NUL UTF-8 bytes unless noted;
their per-field/aggregate bounds are checked before copying. Unknown kind/opcode,
illegal zero/correlation or extra bytes fail PROTOCOL before any admission.

| Kind | Exact body / static role |
|---|---|
| UI_COMMAND | opcode u8, observed generation u64, observed sequence u64, opcode suffix below; controller→coordinator only |
| COMMAND_RESULT | result signed i32 (known Cog code), result state u8 (pending1/final2), related agent u64 (0 absent); server→UI, reply_to exact command/HELLO |
| COMMAND_ACK | empty; UI→server, reply_to exact final result envelope; receipt record identifies original command |
| REPLAY | exact CGR1; server→UI DATA/GAP, UI→server CURSOR only; DATA body is CGV1 full projection, no raw unanchored app chunks |
| BOOTSTRAP | purpose u8(initial1/child2/handoff3/reload4), then exact CGB1 resolved profile below; coordinator→authenticated worker only; child purpose requires exact admitted agent/attempt, others agent/attempt0 |
| BOOTSTRAP_ACK | result i32, configuration generation u64, state u8(copied1/published2); worker→coordinator, reply_to exact bootstrap; copied is not task/guard execution, published is R12 publication not activation |
| CHILD_START | session parent string, task string, pinned configuration generation u64, absolute deadline u64, group u64, member count u16 then exactly those member agent u64 IDs; parent→worker, exact admitted agent/attempt/owner; copied task only once |
| CHILD_CANCEL | absolute drain deadline u64; coordinator→worker, exact attempt; duplicate never extends deadline |
| CHILD_UPDATE | one exact CGA1 update, agreeing with outer agent/attempt/owner; worker→coordinator |
| UPDATE_RECEIPT | exact55-byte private receipt; coordinator→worker; all outer/inner identities agree; not durable parent TOOL-save proof |
| CHILD_MESSAGE | destination agent u64, copied text string; sender exact current child attempt; coordinator relays only same current fan-out group/parent, to admitted bounded mailbox |
| MESSAGE_RESULT | known result i32, state u8(pending1/final2); reply_to exact CHILD_MESSAGE; OK means one mailbox copy, not execution/new turn; AGAIN is provisional, retains same pending wire |
| EXECUTOR_VIEW | mutation serial u64, exact owner epoch/instance u64,active/published config u64 then CGD1 bytes; current primary executor→coordinator; serial separate from canonical CGR cursor; old owner cannot publish after handoff |
| VIEW_COMMIT | mutation serial u64, exact owner epoch/instance u64, canonical generation u64, through_sequence u64; coordinator→exact executor; no effect authority beyond unblocking this matching retained view |
| HANDOFF | exact CGH1; coordinator/source/target role+phase table and exact tuples additionally checked |
| DELEGATE_START | parent owner epoch/instance u64,reservation ID u64,pinned active config u64,STARTED checkpoint u64,assistant node u64,immutable call ordinal u16,call ID string,absolute deadline u64,task count u16 then task strings; remote PRIMARY→coordinator only, never child or UI |
| DELEGATE_RESULT | group u64,aggregate outcome i32,submitted count u16 then member presence u8:1=actual child(agent/attempt u64,status u8,outcome i32,session string,result string,effects-uncertain u8);0=uncreated member negative outcome i32 only; coordinator→primary, reply_to exact delegate start, immutable final output |
| DELEGATE_RESULT_ACK | group u64, parent result checkpoint u64; primary→coordinator only AFTER exact TOOL result+terminal ledger durable save, not CGA/local ACK |
| MAILBOX_POLL | empty; current child→coordinator, exact identified receive tool wire request; copied bounded mailbox batch consumes once |
| MAILBOX_RESULT | result i32, count u16 then source agent u64 and text string; exact poll response, cached bytes; no new turn or policy input |
| EXECUTOR_COMMAND | original client incarnation/request u64, current owner epoch/instance u64, executor prompt serial u64(0 except POLICY), then exact UI_COMMAND body; trusted coordinator→current primary or exact child CANCEL only |
| EXECUTOR_COMMAND_RESULT | result i32,state u8(pending1/final2); reply_to exact EXECUTOR_COMMAND; same immutable duplicate/final-copy/COMMAND_ACK rules, no double input admission |
| POLICY_STATE | presence u8(pending1/retired2), executor prompt serial u64, assistant node u64, call ordinal u16,pinned generation u64,absolute deadline u64,static tool name string; exact current executor→coordinator; duplicate pending serial reuses nonce, retired serial cannot reappear |
| RESYNC_VIEW | one complete exact CGV1 committed snapshot; coordinator→UI only after discovery+explicit CGR1 GAP, replaces at its own anchor, no input/effect |
| DELEGATE_RESERVE | parent owner epoch/instance u64,pinned config generation u64,node u64,ordinal u16,call ID string,absolute deadline u64,count u16,task strings; remote PRIMARY→coordinator BEFORE STARTED,no task effects |
| DELEGATE_RESERVATION | result i32,reservation ID u64(0 on refusal),deadline u64; held global slots/request credit,not permission/execution |
| DELEGATE_RESERVATION_RELEASE | reservation ID u64; release unstarted grant or cancel started group,idempotent,no refund for started/unknown usage |
| CHILD_START_ACK | result i32; worker→coordinator,reply_to exact CHILD_START; task admitted once,not completed |

UI opcodes1..9 are SUBMIT, CONTINUE, POLICY, CANCEL, RELOAD, HANDOFF, SHUTDOWN,
RESYNC,FORK. SUBMIT suffix is text string. During active work it queues with K's
`continuation_authorized=false`; delayed failure cannot promote it to fresh
continuation. CONTINUE suffix is exact incomplete leaf node u64 then fresh text;
observed canonical generation/sequence AND incomplete identity must still match
at dispatch. It cannot be prequeued during an unrelated active turn. POLICY is
session string, assistant node u64, immutable call ordinal u16, prompt nonce u64,
decision u8(deny1/allow2); require exact currently published prompt at the observed
anchor. ALLOW is neither a prefix command nor a persistent policy change. CANCEL
suffix is agent u64(0 primary); correlated active task only, otherwise explicit
refusal. RELOAD is key u8(model1/base-url2/credential-ref3) then value string;
resolver authority belongs to this ONE explicit owner call and is never retained
or forwarded from a worker. HANDOFF is target managed incarnation/number u64
(both0 local), source must be idle/complete; no active attempt migration. SHUTDOWN
and RESYNC have empty suffix. Watchers can request RESYNC but never any mutation.
RESYNC cursor is the envelope's observed generation/sequence.
FORK suffix is node u64, complete current same-session replay-safe prefix only;
matching current observed anchor and idle owner required. Uses existing L
same-session fork logic with no copy/reexecution; incomplete continuation still
requires CONTINUE. /history displays current committed view; /tools displays
current pending policy rows only in the attach profile, /sessions/offline recovery
remain existing standalone commands. This bounded client does not claim full
remote recovery/query UX. /fork reaches typed FORK, never becomes a model prompt.

POLICY_STATE is PRIMARY only. Children use existing K noninteractive ASK denial:
identified no-effect TOOL result before STARTED, no child approval UI/wait. Their
fixed policy never becomes ALLOW from parent delegate approval. Coordinator
assigns one global nonce for
an exact pending executor prompt serial and commits it into CGV1. Forwarded
POLICY carries that serial+nonce; actual owner rechecks its exact PROPOSED call
before STARTED. Retired/missing prompt or deadline never authorizes execution.
EXECUTOR_COMMAND is deduped at both coordinator and executor; ACK means copied
command result, not a new input/model turn. Only root accepts SUBMIT/CONTINUE/FORK;
children receive CANCEL only. No UI/control string can select child config.

Known result wire codes use current result.h values0 and-1..-18. Structural
codecs do not call nested decoders; static ingress uses the exact nested codec/
schema and kind/role table before retaining or dispatching. Oversized input is
LIMIT, malformed input PROTOCOL, stale/foreign identity is refused without state
change. Primary error takes precedence over diagnostic/cleanup error.

DELEGATE_START uses the authenticated current PRIMARY executor binding and exact
parent guard/STARTED call correlation, not an arbitrary child claim. Trusted
executor code sends only after its actual K guarded STARTED checkpoint. The
coordinator's registry owns all created records and its group gate prevents any
SPAWN handler from executing a prefix until full reservation succeeds. If the
nth single-record agent_spawn fails, prefix remains nonexecuting rollback work:
cancel, dispatch/release intent leases, apply/copy/ack terminal failure, release.
No new batch API/hidden blocking rollback; capacity remains charged until actual
cleanup. A started group may produce partial effects on later failure, reported
honestly in immutable results. Root private delegate tool schema takes only
bounded task strings; host chooses placement/permissions/limits. No UI command
can bypass the parent's identified durable tool STARTED barrier.

Terminal CGA notification is independently copied/queued before local ACK and
remote semantic receipt. Parent delegate output remains retained separately
until actual parent TOOL save/DELEGATE_RESULT_ACK. Lost result/ACK resends the same
bytes, never creates another group or consumes mailbox again. Group/result ID
exhaustion reserves final cleanup identities and refuses new tasks. Parent crash
has no volatile-prefix restoration protocol: old worker attempts freeze/drain;
uncertain descendant effects remain identified recovery-only, never auto replay.

## Resolved private bootstrap and wiping ownership

Parent creates a separate immutable profile for each executor under fixed J
bounds; R12's two owners apply PER executor, and aggregate child slots bound all
cloned profiles. Native workers receive only trusted control socket, parent PID
and managed incarnation/number through argv, no task/secret/environment credential.
Worker first compares kernel server credentials with the trusted startup parent
and native getppid snapshot. Parent admits only live unreaped spawned PID+UID plus
exact managed identity. Worker lifetime has no fresh-peer reattachment.

CGB1 is magic4, pinned configuration generation u64, tool policy u8(ASK0/DENY1/
ALLOW2), enabled tool mask u8(read1/search2/edit4/command8), output tokens u32,
tool rounds u32, turn ms u32, tool ms u32, tool output bytes u64, context bytes
u64, retries u32, retry elapsed ms u32, then SEVEN length-u32 fields in order:
model, endpoint name, HTTPS base URL, resolved credential bytes, credential
reference (provenance only, never resolved by worker), workspace root,
shared store root, followed by probe session string, owner epoch u64, owner
instance u64 and exact checkpoint generation u64 (all nonzero), then fixed net
connections u32,connect ms u32,stall ms u32,request bytes u64,header bytes u64,
chunk bytes u64,chunks per request u32,service quantum u32,and CA path string
(length0=system trust). All serialized net settings resolved nonzero and bounded
by startup resource ceilings; retries/backoff0. No callbacks/dependency/global-init
flags or environment-proxy settings are serialized. Pure transport_decode
validates the whole frame, copies only this template/CA path before net_new;
worker supplies its ACTUAL explicit curl initialization/dependency acknowledgement
and its owned observer, then constructs the resolved configuration. Optional
workspace length0 disables tools. Other fields
nonempty; credential is opaque non-NUL bytes (not Unicode), maximum16383 bytes.
Credential bytes must also satisfy J's exact predicate: each byte>32 and!=127;
reject whitespace/control/NUL before provider construction. Other strings obey
J validation/max4095 bytes; aggregate nonsecret copied strings≤64KiB, credential
in separate≤16KiB secret reservation and CA path≤4095 bytes. All limits are nonzero
except retries may be0, u64 byte limits must fit SIZE_MAX and existing checked app
allocation/provider arithmetic. Model is valid UTF-8 token, endpoint exactly
openrouter, base URL satisfies J's HTTPS/no-userinfo rules, credential reference
is copied provenance only (env: references still require valid NAME). Tool policy
and mask are known enum/subset; no startup selectors/borrowed callback/script.
Profile is REAL/OpenRouter only initially; remote MOCK/unknown provider is
UNSUPPORTED before worker/task admission, never a fallback. Local MOCK still uses
explicit borrowed test script data with its documented host lifetime. Literal
request fixture scripts are not serialized as production protocol.

Workspace/store paths are canonical absolute trusted same-host paths resolved
from validated startup directories, not child model choices. The supported host
keeps those configured directory identities stable (no external rename/rebind)
while executors exist; this is not protection against a violating same-UID host.
If canonical identity
cannot be established through the supported host profile, bootstrap refuses.
The worker validates the fixed shared-store profile by exact existing root-session
checkpoint/owner probe supplied at trusted initialization, before accepting jobs.
This is an explicit caller-owned G read after bootstrap_new, which does no IO:
load the immutable exact probe token with INSPECT and independently confirm current
root owner tuple. Do NOT compare probe token to latest checkpoint; normal active
turn saves may advance it. Initial workers finish bootstrap before root startup
input; later child/reload profiles use the stable current owner while admission
excludes handoff. Token proves configured shared-store bytes, never execution
admission authority. Probe is read-only and never acquires/adopts root. Handoff cannot
select another store/workspace through CGH1. Child effective tool mask/policy and
finite limits are intersected with the parent; timeout inherits remaining absolute
deadline, zero is never an unlimited reset. Root ASK granted for delegate does not
become child ALLOW. Explicit global ALLOW may be inherited, but child-only ALLOW
cannot be invented. Child models have no delegate tool (depth1).

Credential direction is ONLY parent→worker BOOTSTRAP. Parent uses a dedicated
constructor-reserved secret body/frame arena, then orchestrator_send's copied IPC
TX queue; that endpoint is constructed with wipe_buffers=true. No orchestrator
incoming event, core event lease, generic receipt-original cache or UI replay
contains credential-bearing frames. Worker directly receives bootstrap into a
dedicated secret arena and uses control_decode with dedicated secret storage,
then copies into an owned J generation. Every layer wipes on logical retirement,
explicit abandonment/free and all failure paths; complete RX remains intact
until receive copies it, even across EOF. Parent/worker caller arenas wipe their
ENTIRE reservation, not just reported used length. The worker's stored config is
the only retained credential copy after admission; config free wipes it after
every provider consuming disconnect return. No raw secret is logged on error.

Bootstrap duplicate proof does not retain a generic raw-secret receipt cache:
while its generation is live, re-encode from the owned profile into dedicated
secret scratch, compare exact immutable bytes/identity, wipe scratch immediately,
then resend ACK. After retirement uniformly stale-reject the old wire. Parent
re-encodes only the same pinned generation while bootstrap ACK is pending; it
never calls resolver again. No ACK means no CHILD_START/effect admission.

Each child admitted under a later pinned generation receives its OWN child-purpose
profile before CHILD_START; a worker's old startup configuration is not silently
reused. Runtime reserves at most one pending bootstrap per executor slot and
retains its owned pinned generation until exact ACK. Primary handoff/reload uses
the same dedicated secret path with distinct purpose. The stable coordinator
retains bounded current/future resolved mirrors for explicit credential resolver
authority; these are copies of at most two logical generations, not extra live
providers/generation identities. An explicit reload resolves once AT COORDINATOR,
then worker execution_reload_resolved enforces only the same three R12 fields,
same transaction/preflight/rollback and cleanup semantics. Neither worker env nor
bootstrap may invent another credential authority. ACK publication does not
claim activation; whole-turn pinning and subsequent visible status report the
actual active generation. Handoff refuses pending publication/activation/notice.

## Executable roles

Private host role parsing precedes existing public option parsing. `--headless
--socket PATH` chooses the persistent coordinator; optional `--workers N` and
`--worker-socket PATH` choose bounded owned capacity. Without workers, children
execute locally. `--attach PATH` is the controller client; `--watch PATH` is
read-only. Attach/watch do not resolve model/config/credentials or construct net,
provider/store. Internal `--worker` accepts only the trusted fixed bootstrap args
above and no normal user/provider/task flags. Role conflicts/unknown flags fail.
Standalone no-role M behavior stays unchanged; installed --socket without the
explicit headless role is refused rather than pretending the old unused listener
is a functional UI protocol. Public app_new's optional listener is unchanged.

Headless root startup prompt/queue are copied/admitted once, then no executor
reads stdin. Server lives until checked shutdown/signal; EOF of a client is detach,
not server shutdown or permission approval. Reconnection in the SAME live client
retains pending command identity; different server incarnation marks it uncertain
and never auto-resends. A new client fetches current state and requires fresh
operator input. No secret/config value echo is added. TTY client renders bounded
full-state replacement with escaped control bytes; nonTTY emits explicit NDJSON
replacement records including anchor, never ambiguous repeated plain-text append.

## Bounded lanes, control leases and compound reservations

Disconnected controller role expires after constructor controller_reconnect_ms
(default3s,max30s); clean detach relinquishes it immediately. Accepted command
receipts remain separate. After expiry a new controller uses fresh incarnation/
IDs and resynchronized prompt state, never inherits queued ALLOW/CONTINUE. Old
client may query retained outcomes read-only but cannot reclaim a replacement's
role. Unacknowledged receipts consume finite slots, never unsafe eviction; new
mutation gets LIMIT while watch/resync/shutdown retain independent reservations.

Lane is encoded after CGL1 kind. Full stream key is(runtime,sender,managed worker,
session,agent,attempt,lane). COMMAND1 handles UI/executor commands/results/ACK;
UPDATE2 CGA/receipts; VIEW3 view/commit; POLICY4 policy state; DELEGATE5 reserve/
grant/start/startACK/result/durable-resultACK; MAILBOX6 message/poll/results;
PROFILE7 bootstrap/ACK; HANDOFF8 CGH1; REPLAY9 DATA/GAP/CURSOR/resync; CANCEL10
child cancel/reservation release. One pending immutable frame/current final
cache+retired watermark PER key, never one whole-worker slot. Another lane's or
sibling's higher wire cannot consume its receipt proof. Reserve all lanes for
maximum executor/client count. Round-robin offers each eligible lane to bounded
IPC copied-send; AGAIN accepts nothing, retains frame but never makes B's poll
wait for A's full-mailbox response. Native CGW1/cancel/final update progress has
independent reservations. Raw socket FIFO still has bounded documented HOL.

Coordinator owns authoritative sibling mailboxes. CHILD_MESSAGE copies source
agent+text into exact current same-group destination record; no unsolicited second
hop/new model turn. Destination's identified MAILBOX_POLL returns source+text once
with cached exact response. Unknown/released/group-mismatched target is refused.

Static owner PREPARE obtains authoritative global reservation after strict local
toolkit constructor validation but BEFORE STARTED. Local reserves directly;
remote exchanges exact DELEGATE_RESERVE/RESERVATION. Bind parent owner/node/call/
pinned active generation/task bytes and original finite deadline. No grant/no
STARTED means no task. Save failure/cancel sends release; deadline/disconnect
retires never-started grant without execution. Only proven unstarted holds refund
credit; started worst-case credit remains charged. Worker new_ alone reserves
local state, never falsely promises remote global credit. Group gate remains
closed until all single-agent records admitted; rollback prefix is nonexecuting
and follows ordinary lease/cancel/terminal-copy/ACK/release cleanup. Result presence
bits preserve created identities and explicitly absent uncreated members. No
fabricated agent/session. Aggregate OK iff all completed OK; any uncertain effect
requires aggregate INDETERMINATE. First known failure cancels remaining members,
preserving already completed factual results in submission order.

Actual active/published configuration facts travel in EXECUTOR_VIEW; delegation
names active generation explicitly. G2-published/G1-active turn clones G1 mirror,
never latest by assumption. Unknown/retired generation refuses. Child conversation
is real durable K state; PRIMARY only uses conversation replay barrier. Child
status/result/policy use CGA/POLICY_STATE, not fake root history or undeclared
child conversation ACK. Core/local ACK, remote receipt and durable parent TOOL
result ACK remain separate. A returned execution borrow is not task admission.

Successful client render sends REPLAY(CGR1 CURSOR), read-only REPLAY lane, exact
session/generation; no client DATA/GAP. Server tracks queued-send separately from
rendered cursor, validates against committed/sent window, advances only on this
ACK, and ignores stale duplicates. Send-OK/COMMAND_ACK never counts as render.
RESYNC acknowledges only full replacement; ordinary render uses CURSOR. Watchers
are allowed. Partially decoded/rendered resync cannot enable input/approval.

## Native worker management and failed handoff

Public CGW1 encode/decode in orchestrator.h is separate from private CGL1. Q frame
type0x7f01 carries native HELLO/heartbeat/shutdown/ACK; type0x7f02 carries CGL1.
Worker_host_step is sole reader and static kind dispatcher, never old hello-PID
text. Orchestrator consumes native management and copies application MESSAGE
events without including application headers. Kernel PID/UID+owned live spawn+
exact managed identity precede READY. Exec environment PATH/LC_ALL only, blocked
INT/TERM reset in child before exec; no parent handler/reaper/mask mutation.

ACTIVATE reply echoes exact outer reply_to/CGH tuples and is retained at target;
lost ACK retries never reload latest/repeat CAS/guard/save. No fresh input forward
until exact activation ACK. Abort/failure leaves source frozen. Initial profile's
supported reclaim is host shutdown/close, then explicit new --resume or --recover
--fenced under L's fresh+1 epoch and actual guard/recovery rules. No in-place UI
reclaim opcode or old-guard thaw. Uncertain effects never auto replay.

## Child context, install timing and failed coordinator disposal

Before first child task/model send, host appends and durably saves a bounded
SYSTEM context record with actual parent session, parent assistant node/full call
ID/ordinal, runtime incarnation, group/agent/attempt, pinned config generation
and actual admitted sibling agent IDs as canonical decimal STRINGS. The original
task follows unchanged as USER input. This supplies actual send_message targets,
never authority to invent an ID, permissions or routing; no PID/secret/URL/path is
included. Strict send_message schema uses decimal string IDs (1..20 digits, no
leading zero/overflow), preserving full uint64 range beyond I's signed integer
subset. Workspace mask0 means no workspace tools, not disabled sibling messaging;
same fixed policy applies to all child calls, ASK denies without STARTED.

delegation_context_new installs exactly once after resolved execution construction
and before begin/restored-PROPOSED admission. Static role-selected schemas exist
before provider preflight; runtime userdata binds only that admitted context. App
owns context after success, destroys retained tool handles first, then clears/
destroys bridge while app is alive, then remaining app resources. No late dynamic
tool registration, pointer into a freed app or second context destructor owner.

If coordinator core has a fatal driver stop during prefix rollback, do NOT restart
its loop merely to dispatch intent leases. Runtime still owns/drives each started
executor's separate core and native worker cleanup, saves actual failed/uncertain
parent result where possible, returns all exact local session borrows, then
existing core_free discards queued intent leases/records without executing them.
No individual agent_release claim is made before its leases actually clear.
Fatal teardown may consume unacknowledged core records only after actual started
work drained/fenced and borrowed sessions returned, preserving primary error.

After parent crash, registry/CGA prefix/receipts/reservations remain VOLATILE;
worker claim is never reconstruction authority. Surviving child parent link and
saved SYSTEM provenance identify actually created execution when present, and
durable parent TOOL result identifies returned members when its save completed.
A crash before those writes can leave unknown placement/member set; this is
explicit unknown effect/recovery state, not a claim all attempts can be enumerated
or their prefix restored. Existing explicit --sessions/inspect/recover/fencing
supports operator investigation; no automatic child replay or fabricated k0.
