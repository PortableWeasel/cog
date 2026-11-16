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
| REPLAY | one exact CGR1 record; server→UI; DATA body is CGV1 full projection, no raw unanchored app chunks |
| BOOTSTRAP | purpose u8(initial1/child2/handoff3/reload4), then exact CGB1 resolved profile below; coordinator→authenticated worker only; child purpose requires exact admitted agent/attempt, others agent/attempt0 |
| BOOTSTRAP_ACK | result i32, configuration generation u64, state u8(copied1/published2); worker→coordinator, reply_to exact bootstrap; copied is not task/guard execution, published is R12 publication not activation |
| CHILD_START | session parent string, task string, pinned configuration generation u64, absolute deadline u64, group u64, member count u16 then exactly those member agent u64 IDs; parent→worker, exact admitted agent/attempt/owner; copied task only once |
| CHILD_CANCEL | absolute drain deadline u64; coordinator→worker, exact attempt; duplicate never extends deadline |
| CHILD_UPDATE | one exact CGA1 update, agreeing with outer agent/attempt/owner; worker→coordinator |
| UPDATE_RECEIPT | exact55-byte private receipt; coordinator→worker; all outer/inner identities agree; not durable parent TOOL-save proof |
| CHILD_MESSAGE | destination agent u64, copied text string; sender exact current child attempt; coordinator relays only same current fan-out group/parent, to admitted bounded mailbox |
| MESSAGE_RESULT | known result i32, state u8(pending1/final2); reply_to exact CHILD_MESSAGE; OK means one mailbox copy, not execution/new turn; AGAIN is provisional, retains same pending wire |
| EXECUTOR_VIEW | mutation serial u64, exact owner epoch/instance u64 then CGD1 bytes; current primary executor→coordinator; serial separate from canonical CGR cursor; old owner cannot publish after handoff |
| VIEW_COMMIT | mutation serial u64, exact owner epoch/instance u64, canonical generation u64, through_sequence u64; coordinator→exact executor; no effect authority beyond unblocking this matching retained view |
| HANDOFF | exact CGH1; coordinator/source/target role+phase table and exact tuples additionally checked |
| DELEGATE_START | parent owner epoch/instance u64, STARTED checkpoint u64, assistant node u64, immutable call ordinal u16, call ID string, absolute deadline u64, task count u16 then task strings; remote PRIMARY→coordinator only, never child or UI |
| DELEGATE_RESULT | group u64, count u16 then submitted-order child records(agent/attempt u64,status u8,outcome i32,session string,result string,effects-uncertain u8); coordinator→primary, reply_to exact delegate start, immutable final output |
| DELEGATE_RESULT_ACK | group u64, parent result checkpoint u64; primary→coordinator only AFTER exact TOOL result+terminal ledger durable save, not CGA/local ACK |
| MAILBOX_POLL | empty; current child→coordinator, exact identified receive tool wire request; copied bounded mailbox batch consumes once |
| MAILBOX_RESULT | result i32, count u16 then source agent u64 and text string; exact poll response, cached bytes; no new turn or policy input |

UI opcodes1..8 are SUBMIT, CONTINUE, POLICY, CANCEL, RELOAD, HANDOFF, SHUTDOWN,
RESYNC. SUBMIT suffix is text string. During active work it queues with K's
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
instance u64 and exact checkpoint generation u64 (all nonzero). Optional
workspace length0 disables tools. Other fields
nonempty; credential is opaque non-NUL bytes (not Unicode), maximum16383 bytes.
Credential bytes must also satisfy J's exact predicate: each byte>32 and!=127;
reject whitespace/control/NUL before provider construction. Other strings obey
J validation/max4095 bytes; aggregate copied strings≤64KiB. All limits are nonzero
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
