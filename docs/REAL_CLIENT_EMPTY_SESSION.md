# Empty durable session admission

Bug `cd3d092`, baseline `84e2272`. A fresh durable constructor admits owner/guard
but writes no checkpoint until first input. Exit before input leaves a valid
owner-only manifest: NEW refuses its ID and RESUME/FORK/RECOVER all fail legacy
load with NOT_FOUND. The existing RECOVER mode cannot repair it. This design
changes only application admission and uses existing G store APIs.

## Pass 1: narrow contract

Successful fresh DURABLE construction commits the empty session snapshot before
publishing the app. It contains its exact ID and no nodes, active leaf, tool ledger
or model/provider provenance. J still resolves current immutable configuration
through its normal startup sources; saving the empty session neither persists
credentials nor makes up historical configuration. Disabled store/LIST remain
unchanged. A later provider/core constructor failure may leave a committed empty
session, which explicit RESUME can load safely.

The crash/failure window between owner publication and first checkpoint still
needs recovery. Explicit RESUME may reconstruct only a valid nonzero owned
checkpoint0 reservation with no replay-loadable data. Existing public calls:

1. owner_read validates the authoritative owner/checkpoint; both zero is absent.
2. load_ex(REPLAY) must return NOT_FOUND; every other load error is preserved.
3. session_new constructs an empty matching-ID session within current reservations.
4. owner_compare_exchange uses the exact old owner/checkpoint0, fresh incarnation
   and checked epoch+1. Locked CAS excludes live guards/stale state and refuses
   legacy state/remnants through the corrected G admission contract described below.
5. owner_acquire confirms the newly admitted normal owner under the lock.
6. Existing checkpoint_save with that exact normal guard commits the empty
   checkpoint before app construction succeeds. Its output token is local; no
   token is invented for the preceding owner-only state.

No owner/data deletion, epoch reset, unowned adoption, effect replay, fabricated
result or implicit recovery-mode downgrade occurs. No new API/option is added.
The existing public RESUME prose gains this precise no-checkpoint exception;
NEW collision and FORK/RECOVER semantics remain intact.

The private `session_admission.h` owns the existing sessions_open declaration and
documents partial construction, exact authority and all error boundaries. No
implementation body is changed in this pass.

## Pass 2: complete scenario trace

This is an admission amendment, not a new turn/recovery/effect mechanism. Each
original scenario crosses the same constructor boundary before runtime work.

| Original scenario | Header coverage and change |
|---|---|
| 01 prompt/stream | NEW/DURABLE owner CAS/acquire -> empty checkpoint_save -> existing user save/send; no fake first message |
| 02 network recovery | No provider before admission; existing per-turn retry and incomplete-save rules unchanged |
| 03 subagent | No new child authority; O runtime remains separate; initial app construction must succeed first |
| 04 cold start | Successful app_new now guarantees a durable empty snapshot even with no prompt |
| 05 shutdown | Idle EOF/INT/TERM/free releases guard, committed empty session remains RESUME-loadable |
| 06 parallel tools | No STARTED at checkpoint0; existing identified durable STARTED/result rules unchanged |
| 07 aux crash | No remote failover change; owner-only restart is exact local CAS/guard and cannot replay work |
| 08 config reload | Empty save invents no config/model/provider provenance; J and R12 generation ownership remain independent |
| 09 peer messages | No IPC/event routing change; no new admission before constructor success |
| 10 fan-out | No orchestrator/child behavior or fake acceptance introduced |
| 11 disconnect/replay | Committed tree/display path unchanged; empty snapshot has zero active/node/high-water identities |
| 12 rate limit | Empty checkpoint precedes wire; app retry budget and effect exclusion unchanged |
| 13 persistence | Exact committed RESUME stays normal; explicit owner-only RESUME reconstructs only the proven no-data reservation |
| 14 fork | NEW collision refusal preserved; FORK requires a real saved node and cannot invent an owner-only source |
| 15 tool turns | Existing committed history/results/opaque/ledger all restore exactly; only checkpoint0 can use empty repair |
| 16 errors | Initial CAS/acquire/checkpoint error returns no app/provider/effect; INDETERMINATE is never guessed success |
| 17 migration | Unowned text-v1 migration/fencing unchanged; owned checkpoint0 with readable legacy is refused |
| 18 observability | No request/tool span for empty persistence; no fabricated model/provider metadata or credential logging |
| 19 diagnostics | No net construction/request from empty repair itself; host dependency lifetime unchanged |
| 20 interrupt/queue | Constructor succeeds with checkpoint before stdin work; queued/fresh continuation authority unchanged |

The checkpoint0 proof is conjunctive. NOT_FOUND from legacy load is insufficient
by itself: exact locked owner CAS and acquire are also required, and G forbids
adopting legacy files under this owner path. A missing nodes file with meta
present is IO; malformed/unreadable meta is PROTOCOL/IO, never an empty proof.
CAS refuses any newly appeared recognized legacy remnant under its lock. If another owner
publishes a checkpoint or epoch between reads, exact CAS fails; it does not load
that new state and retry automatically. A live normal/recovery guard returns AGAIN.

Stored nonempty nodes already carry model/provider provenance and remain on the
ordinary exact-load path. No config file or environment value is copied from an
owner tuple or checkpoint number; current J sources remain the explicit ordinary
RESUME configuration authority. Empty reconstruction has no nodes to attribute.
No --fenced assertion can bypass malformed data, a live guard or failed save.

## Pass 3: consistency, failure and feasibility

Independent review found a real G gap: baseline legacy load stops at missing
meta.json and checkpoint0 CAS inspects only that file, so lone nodes.jsonl can
escape the proposed no-data proof. App implementation is gated on separate
reviewed correction `9a95735`, rather than an app-side filename inspection or
weaker proof. The finite recognized legacy remnants are meta.json, nodes.jsonl,
index.bin and each .tmp counterpart. Their presence at checkpoint0 causes
conservative refusal; it is not a claim that a staged/index file is committed
authority. Inspection errors other than ENOENT fail IO. Unrelated unreferenced v2
generations/.generation.tmp/.manifest.tmp retain existing nonauthority semantics;
an actual valid v2 checkpoint remains authoritative over stale legacy remnants.

Only RESUME with validated nonzero owner/checkpoint0 and load_ex NOT_FOUND can
take the empty repair. Every other load result stays on the current success or
error path. In particular successful legacy load retains existing migration/
refusal rules; this exception does not silently convert legacy into an empty v2.
An exact CAS/acquire failure never falls back to another read/checkpoint/epoch.
If another process wins, the caller explicitly restarts construction to inspect
that state. A fresh incarnation always advances the epoch, never impersonates
the old owner or conflates store generation with J config identity.

| Boundary | Required result and permanent oracle |
|---|---|
| fresh durable idle | app_new returns only after nonzero checkpoint; free/EOF/signal then RESUME gets same empty identity and fresh owner |
| normal disabled store | No checkpoint/store/guard added; text fixture semantics remain unchanged |
| owner-only interrupted process | Seed public owner/guard at0, let process exit; explicit RESUME creates empty checkpoint with a different incarnation and epoch+1 |
| existing live guard | RESUME returns AGAIN; no checkpoint, owner change, provider send or effect |
| absent source | Owner0/checkpoint0 never bootstraps through RESUME; NOT_FOUND and no new source |
| NEW/FORK/RECOVER selectors | Existing NEW ID refuses; owner-only FORK/RECOVER cannot invent a source; no effect reconciliation is guessed |
| initial checkpoint IO | *out=NULL; owner may remain reserved, no input/model/tool work; later explicit RESUME can repair |
| initial checkpoint INDETERMINATE | *out=NULL whether manifest remains0 or committed checkpoint is present; next explicit RESUME reads authority rather than guessing/undoing it |
| owner CAS uncertainty | No acquire/save/model after nonOK CAS; durable publication may remain, never reset |
| failed acquire or save | App consumes partial resources/guard; admitted epoch may remain; next construction uses current exact authority |
| stale CAS | Intervening owner/checkpoint refuses, no overwrite and no internal retry |
| epoch exhaustion | LIMIT with no wrap or invented owner; no checkpoint/work |
| legacy remnants/error | G correction refuses recognized artifacts and inspection IO; app propagates result, never private-peeks/resets |
| committed v1/v2 | Existing migration/assertion and exact-checkpoint/uncertainty behavior remains; model/provider/opaque/ledger node data is preserved |
| current config provenance | Different current J model/config sources do not overwrite saved node metadata or infer model from owner/checkpoint; empty snapshots have no invented nodes |

An empty session has zero nodes rather than a fabricated root message. Existing
public snapshot/checkpoint codecs support this shape (active/high-water/ledger0).
checkpoint_save is an explicit existing operation under the normal guard before
any borrowers; no app buffer growth or new transaction API is introduced.
Save failure can leave an owner or checkpoint durable; it never makes the ID
available for NEW reuse. All outer app_new failures retain NULL output and run
ordinary matching resource cleanup. There is no successful intermediate app to
accept input, and no rollback/delete of possibly committed persistence.

Three header passes and a different final reviewer are required before app
bodies. G correction9a95735 must also pass before the dependent repair body.

## Reviewed implementation and concrete fixtures

Private/public prose passes `67552f3`, `d20ecd0`, `b5f327d` precede app bodies.
Different final header review passed exact `b5f327d` with22 independent strict
GCC/Clang C11 and G++/Clang++ C++11 checks; writer checks passed14. G correction
`9a95735` independently passed source `6f5aacc` and is integrated through main
`f2d9159`. Its [evidence](REAL_CLIENT_LEGACY_ADMISSION.md) records the finite
legacy-remnant refusal and separate52-test compiler/sanitizer checks.

Application production checkpoint `ccb77a0` changes only app_sessions.c. Fresh
durable admission and the exact owned/absent checkpoint0 RESUME path call existing
checkpoint_save under their matching normal guard. All errors propagate without
publishing an app. A new explicit RESUME re-reads any owner/checkpoint that remains;
neither branch deletes/reset IDs, fabricates provenance or starts provider work.
Ordinary committed and legacy load paths retain their existing bodies.
Approved main/83 host output fixes and G history are preserved through merge
`7e1457e`; no sibling implementation or public ABI was changed by the app fix.

Fixture checkpoint `9eadf21` supplies two committed tests:

- `cli-empty-session`: empty initial checkpoint/ordinary restart with zero mock
  sends; actual child owner death and live-guard exclusion; NEW/RESUME save failures
  before and after publication for both IO and INDETERMINATE; exact CAS uncertainty,
  a real competing owner CAS, acquire failure and epoch exhaustion; selectors,
  collision and recognized remnant refusal; stored historical model/provider data
  unchanged while J selects current configuration.
- `cli-installed-empty-session`: normal runtime-component install; fresh idle EOF
  and empty history; SIGINT/SIGTERM with actual core/mask readiness and stdin kept
  open until exit; subsequent guard acquisition/resume; old owner-only reservation
  repair. Public snapshot inspection verifies zero nodes/ledger/high-water and
  exact epoch/checkpoint state. No model/effect bytes execute during repair.

The original fresh constructor without this fix fails the prepared checkpoint
assertion; the composed source passes both new GCC Release tests. Initial-save
faults include ordinary IO after a real successful publication as well as
INDETERMINATE, so uncertainty is never reduced to one error spelling. Existing
first-user-save injection semantics remain intact because initial persistence
uses the explicit checkpoint operation, not ordinary turn save.

## Writer validation and final review handoff

All-target strict GCC14 Release, Clang19 Release and Clang19 RelWithDebInfo
ASan/UBSan builds pass. The same **56/56** selected CTests pass for each profile
(**168/168** total), including both new fixtures, existing session/store/fencing
tests, all CLI tests, original08/13/14/15/17/20 and the preserved output/ASK and
observability regression fixtures. No assertion policy or unrelated package is
weakened. ASan enables leak detection and halt-on-error; UBSan enables
halt-on-error/stack traces. Build/log names are `build-empty-{gcc,clang,asan}`
and `/tmp/cog-empty-{gcc,clang,asan}-{configure,build,tests}.log`.

Each fresh profile uses the accepted final installed dependency artifact and
its genuine audited cache:

```sh
cmake -S . -B BUILD -DCOG_ENABLE_WERROR=ON -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/tmp/cog-relocatable-profile/shipped/curl-build/install \
  -DCURL_DIR=/tmp/cog-relocatable-profile/shipped/curl-build/install/lib/cmake/CURL \
  -DCURL_LIBRARY=/tmp/cog-relocatable-profile/shipped/curl-build/install/lib/libcurl.so \
  -DCOG_NET_CURL_BUILD_DIR=/tmp/cog-relocatable-profile/shipped/curl-build \
  -DCOG_NET_CURL_LIBRARY=/tmp/cog-relocatable-profile/shipped/curl-build/install/lib/libcurl.so
cmake --build BUILD -j5
ctest --test-dir BUILD --output-on-failure -j4 \
  -R '^(unit-session.*|unit-store.*|cli-|scenario-test_(08|13|14|15|17|20))'
```

Clang adds `-DCMAKE_C_COMPILER=clang`; a separate ASan directory uses
`-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCOG_ENABLE_ASAN=ON` and runs CTest with
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` plus
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. The installed runtime-component
cases use explicit MOCK and claim no independent real-transport packaging closure;
that remains the separate R18 fixture. Third-party shared dependencies are the
approved ordinary Release builds.

Different final implementation review passed coherent checkpoint `fef09ab` in an
isolated managed review worktree. Independent all-target strict GCC14.2 Release,
Clang19.1 Release and Clang ASan/UBSan/LSan builds passed with the admitted shipped
dependency profile. Each profile passed 54 store/session/CLI checks plus four
additional original scenarios (08/14/15/20): **58 unique checks per profile,
174 total**. This includes both committed empty-session fixtures.

Six independent optimized negative controls removed fresh checkpoint publication,
disabled owner-only repair or allowed absent-owner adoption, under both GCC and
Clang. Every variant failed its intended fixture assertion (lines 97/126/107).
An additional real constructor failure after checkpoint publication (invalid log
destination) preserved the committed empty snapshot and released its guard; a
later explicit RESUME advanced the epoch without changing the checkpoint. That
probe passed under all three profiles. Review logs and reproducible temporary
probes are retained under `/tmp/cog-empty-review-{gcc,clang,asan}`,
`/tmp/cog-empty-review-negatives` and `/tmp/cog-empty-review-probes`; ticket
`cd3d092` records the exact independent verdict. No production or fixture change
was required by final review.

The approved app branch was integrated with a no-fast-forward merge and the
independent review evidence retained on main `111ae0b`. Its main-sourced strict
Release all-target build and all **58/58** affected checks passed, including both
installed/constructor fixtures; the accepted dependency cache was unchanged.
Postmerge logs are `/tmp/cog-empty-postmerge-{build,tests}.log`.

This focused fix does not close M, O, all38 or release P. No paid requests.
