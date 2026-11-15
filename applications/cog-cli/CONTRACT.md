# cog-cli application contract

Stage 14.K composes the integrated core, provider, toolkit, store and J
[effective configuration](CONFIGURATION.md). Its independent review and exact
verification scope are recorded in [K evidence](../../docs/REAL_CLIENT_K_EVIDENCE.md).
This is the single-process conversation/tool client. L adds session selection,
terminal history/usage and explicit offline recovery, with its own header gate
and independent review. Remote IPC, replay and subagent application wiring remain
N/O work.

The executable parses options and explicitly initializes the dependency runtime
and audited c-ares-only transport for REAL. It blocks SIGINT/SIGTERM and uses a
borrowed signal file descriptor serviced by the app, without a mutable global
signal-handler pointer. Tool helpers reset inherited masks and dispositions.
Teardown drains the app, releases transport, cleans up the dependency runtime,
closes the signal descriptor and restores the host mask. Embedders instead
provide `opts.net` with their own explicit dependency lifetime.

REAL is the default and requires an explicit model and usable credential
reference. Configuration errors never select MOCK. `--mock` is an explicit
fixture selection. CLI > environment > config file > defaults and presence bits
are defined by CONFIGURATION.md; programmatic tests must set presence bits for
their intended overrides. No credential or arbitrary provider diagnostic string
is included in app error messages.

The private implementation is split into `cli.c` (ownership/construction),
`app_turn.c` (static dispatch, service and input), `app_tools.c` (durable tools),
`app_sessions.c` (selection, guarded restore and terminal recovery),
`app_display.c` (snapshots/CGD1), and `main.c` (host composition). J's immutable
configuration generation and secret remain alive until provider disconnect.
All contexts are explicit; dispatch and tool descriptors are static const.

| State | Work |
|---|---|
| IDLE | Copy/validate user input, save user node, build bounded history, send |
| MODEL | Poll generic provider and consume ordered typed deltas |
| BACKOFF | Wait on one application-owned retry timer while servicing input/stop |
| POLICY | Validate complete identified calls and apply explicit per-call policy |
| TOOL | Poll one asynchronous execution through bounded capture and child reap |

The core service interval is four dispatched events, with a 10 ms idle ceiling
reduced by provider/tool/application deadlines. Provider data is retained in app
scratch if the core queue is full. Internal work events carry a request serial,
so a queued event from a stopped request cannot consume a later turn's data.

Constructor reservations include 1024 session nodes, 16 tool calls per assistant,
16 complete opaque blobs, eight copied input slots, context-sized text/opaque
scratch, provider delta storage, history/display arrays and tool-result capture.
Arguments are bounded to 65535 bytes per call; IDs/names to 255 bytes. Session
payload is reserved at four times the configured context bytes. Explicit async
tool constructors reserve their handles before effects; the app never grows
its own buffers during service. Dependency allocation semantics remain those of
the approved transport profile.

Streaming text is printed and accumulated separately from durable history. One
assistant node is inserted for each finished model turn. Only successful FINISH
with STOP or TOOL_CALLS produces COMPLETE; LENGTH/filter/error preserves an
explicit INCOMPLETE aggregate, cancellation a CANCELLED aggregate. Complete
opaque continuation and reported usage survive insertion and resend. Usage
snapshots replace prior values, with unknown billing remaining unknown. Partial
tool fragments never become executable calls.

Incomplete/cancelled turns cannot be replayed. Earlier queued input, including
`--queue`, remains retained and cannot authorize a branch change or another send.
Fresh explicit `app_submit` while the failed turn is inactive, or fresh
interactive input, authorizes a fork at the last safe ancestor and a newly
appended user message. The failed aggregate remains on its old branch. A
successful explicitly authorized continuation may then process retained queued
work. `app_queue` by itself never grants that continuation authorization.

One retry budget covers the whole user turn, including model/tool rounds:
default one additional attempt, bounded retry elapsed time and overall turn time.
Retry requires generic retryable + replay_safe error metadata, no accepted app
output and no effects. Transport does not retry. Backoff is a timer, never a
sleep in a handler. Exhaustion reports the original error without another send.
Context is the sum of visible text, tool identity/arguments, opaque continuation
and advertised schemas; protocol serialization has separate provider limits.
No context truncation, token-count guessing or model substitution occurs.

REAL defaults to a durable XDG/HOME store. Every required parent directory is
created without following symlinks and new directory entries are fsynced before
store construction. Durable MOCK fixtures use the same fenced store profile.
NEW creates fresh sessions and refuses an existing durable identity; it does not
replace history or impersonate an old incarnation. RESUME/FORK explicitly select
existing data through the guarded workflow below. The guard remains borrowed by
all executions until their terminal destruction.

Tools are advertised only with both a workspace and durable store. Explicit
no-store is text-only; unsolicited calls receive identified DENIED results even
with ALLOW policy. ASK waits for `allow`/`deny` in interactive mode and denies in
noninteractive mode. ALLOW is explicit autonomous opt-in. Strict built-in
read/search/edit/command schemas and descriptor-relative workspace checks still
apply. Commands receive only PATH=/usr/bin:/bin and LC_ALL=C.UTF-8; credentials
and the host environment are not inherited. Command permission is not OS
confinement.

Each allowed call transitions PROPOSED → STARTED and saves durably before
`execution_start`. The terminal result and ledger transition are saved together
before another model request. Denied/invalid calls receive matching no-effect
results. Results include status, truncation, possible effects and exit-status
availability. Round exhaustion closes unstarted proposals by denial and makes
no further model request.

Any INDETERMINATE save immediately freezes new input/model/tool admission.
An executed tool's terminal handle and result remain retained if insertion,
transition or save fails; drain returns that original failure, does not construct
another execution, and later run/input admission is refused. An uninserted
result is included as an incomplete visible TOOL snapshot item. Caller teardown
may release the already-terminal handle after observing the failure. Restart
uses the store's explicit indeterminate-recovery contract; K never guesses or
repeats an effect.

`app_run` cancels and drains provider/tool work on every stop/error exit. Process
cleanup follows the toolkit's owned process-group TERM/KILL/reap profile.
`app_stop` only forwards to the signal-safe core stop. `app_free` requires no
active execution; prepared and terminal handles are releasable. Provider,
workspace, guard, session and dependency lifetime ordering remains explicit.

`app_display_snapshot` returns the selected branch plus active/incomplete visible
state at one session/generation/through_sequence anchor, independent of replay
eligibility. Opaque data is excluded. CGD1 encodes big-endian integers and exact
UTF-8 text, with no native structures. Encode/decode capacity failure is
transactional; malformed roles/statuses, UTF-8, lengths, generation or trailing
bytes fail. O's CGR1 reconnect/GAP protocol consumes this snapshot contract later.


L session selection uses the public session_action and source_session_id fields:
`--sessions`, `--resume ID`, `--fork ID --from-node N`, or `--recover ID --fenced`.
These selectors are explicit CLI/host inputs, outside configuration precedence.
NEW alone accepts `--session ID`. Every non-NEW action requires DURABLE; DEFAULT
becomes durable even for MOCK. LIST/RECOVER bypass credentials, net, provider,
workspace and core creation. Malformed present configuration remains an error.
LIST reports sorted metadata names, with no claim of generation integrity or
replay health. Its temporary `cog_store_list` output constructor can allocate
proportionally to enumeration; there is no fixed pre-enumeration byte ceiling.
Before any output, IDs must all be valid UTF-8/store-safe 1..127-byte names, at
most 1024 entries, and fit aggregate max_context_bytes. No partial/truncated
successful list is returned. A missing store lists empty without creating it.

RESUME reads owner/checkpoint, loads that exact immutable generation, restores
current explicit app reservations, admits a fresh incarnation/checked epoch and
acquires a matching exclusive guard. A live guard or stale checkpoint refuses
activation. Completed ancestor effects are never reconstructed as new executions.
A partial text turn retains K's fresh-input continuation rule. A final assistant
with PROPOSED calls receives no-wire provider preflight over the full selected
prefix, including any already-recorded results and current model/tools/settings,
before policy admission. Already closed calls are skipped. Actual later send
revalidates completed history. Unknown/unsupported or malformed continuation
fails before a remaining tool starts; no synthetic results are manufactured.

FORK selects an existing replay-safe point in the same durable session tree.
The next fresh message is explicitly added at that parent; other branches,
node IDs, tool/results/audits and opaque bytes remain intact. Selecting a nonleaf
alone does not overwrite the active leaf or durably create a branch. Active or
queued work refuses selection, and unresolved/unsafe history cannot be forked.

Unowned v1/v2 adoption requires `--fenced`, the operator assertion that old writers
are quiescent and prior command processes stopped/reaped or externally fenced.
The application does not verify or perform that cleanup from an opaque owner ID.
Text-only v1 migration uses a temporary unowned store for explicit v2 publication,
releases it, then activates a fenced owner against the exact resulting checkpoint.
Unidentified legacy tools and unowned uncertain effects are refused. Already-owned
uncertain v2 uses only RECOVER plus its explicit assertion and a recovery guard.

Recovery is offline. `/tools`, `/history` and
`/reconcile NODE CALL succeeded|failed|cancelled|acknowledged EVIDENCE` identify one
specific call and operator statement. For arbitrary IDs, `/reconcile-index NODE
ORDINAL OUTCOME EVIDENCE` uses the 1-based original assistant tool_calls order,
which never changes when earlier entries close. `/tools` escapes IDs and reports
these stable ordinals. Literal CALL is never reinterpreted as an index, including
IDs that look like `@2`. Acknowledgement explicitly preserves unknown
effects. Ordinary text, policy ALLOW, queued input, fork and cancel cannot execute
work in recovery. Only successful result+audit save reports reconciliation success;
failed or uncertain save freezes later reconciliation and retains the guard until
teardown. Even after all calls close, the app stays recovery-only. A later explicit
RESUME admits another epoch and normal guard before ordinary work.

Normal terminal commands add `/sessions`, `/history`, `/tools`, `/fork NODE` and
`/usage` beside K's `/cancel` and `/quit`. Entire input lines are validated as UTF-8
without NUL before command parsing. Known usage is reported per assistant node;
unknown availability is stated rather than billed as zero. `--usage` reports known
turn usage to stderr while streamed stdout stays text-only. Output and flush
failures propagate, including unknown-usage output. Recovery handles invalid/error
input descriptors as IO and honors the app's constructor-verified lock-free stop
flag without requiring a core. Main suppresses SIGPIPE for its explicit lifetime
so EPIPE reaches cleanup, then restores its prior disposition.

The `cog-cli-runtime` install component installs the actual executable for the
offline installed-binary restart tests. Shipped real transport dependency closure
remains P's packaging gate; installing this component alone makes no such claim.
