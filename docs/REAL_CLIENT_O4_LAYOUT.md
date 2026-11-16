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
| PROFILE secret exchanges | one startup per worker plus one per executor; dedicated body/frame/scratch; pinned generation owner remains live | full-capacity volatile wipe on retirement/abandon/free, not generic receipt cache |
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
