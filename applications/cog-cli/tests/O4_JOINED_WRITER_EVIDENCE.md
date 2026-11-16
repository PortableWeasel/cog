# O4 joined writer checkpoint

Production checkpoint before final startup review: `6fce3a8` (approved consuming handoff cleanup correction),
with maintained test/registration corrections through `7f4d613`. This is writer
evidence for independent review `df377bf`, not a module acceptance or merge.
Main and the integration checkout were not modified.

The joined application uses the actual executor, core, provider, toolkit, store,
IPC, orchestrator, codecs, projection, coordinator and native worker. The loopback
TLS fixture uses the real REAL-provider path and built worker role, not a runtime
stub or paid provider. Its child commands create distinct factual effect files.

## Validation inventory

All build trees/logs are under this checkout's `build-o4-*` directories. Builds
used strict warnings/Werror and at most two jobs. Project libraries and fixture
executables are instrumented in the sanitizer profiles; the admitted external
curl/c-ares shared dependencies are not instrumented. ASan/UBSan halt on findings
and enable leak detection; TSan halts on findings.

| Profile | Whole-suite baseline | Final changed-scope checks |
| --- | --- | --- |
| GCC | 162/163 at `5e24284` | 13/13 cleanup/executor/handoff/native; 2/2 corrected integration fixtures; 2/2 newly registered view fixtures |
| Clang | 162/163 at `5e24284` | 13/13 cleanup/executor/handoff/native; 2/2 corrected integration fixtures; 2/2 newly registered view fixtures |
| Clang ASan/UBSan/LSan | 164/165 at `ec735ca` | 15/15 cleanup/executor/handoff/native and corrected integration fixtures |
| GCC TSan | 163/165 at `ec735ca` | 15/15 cleanup/executor/handoff/native and corrected integration fixtures |

The common baseline failure was the old malformed-frame test's requirement to
dispatch DISCONNECTED after a fatal LIMIT retired its endpoint. Independent
review approved fixture-only correction `a4973f0`, imported as `edf7aa2`: actual
peer closure, fatal result/done/latch, retained queue ownership and clean rebegin
are asserted; ordinary EOF disconnect assertions remain. No core source changed.
The second TSan failure was missing TSan linkage in the separately built installed
consumer. `bc43972` matches its instrumentation to the producer. This correction
also puts that real consumer build under its explicit producer build directory;
normal install, two relocations, exact dependency hashes and foreign-path/loader
audits remain enforced. `578460e` applies the same compiler-scratch policy to the
allocation-negative diagnostic and optional live-client tool workspace; those
two runners received syntax checks only, with no live invocation.

Independent path-audit negatives found and corrected an overbroad owned-prefix
exception in the test-only scratch relocation. `46102b8` and `7f4d613` preserve
rejection of the old staged prefix and prefix-sibling paths while allowing only
the actual owned subtree nested beneath a producer ancestor. The registered
negative/positive path audit and actual installed consumer pass 2/2 in all four
profiles after that correction (`audit-final-check.log`).

The pre-startup-review registry has 167 tests. Baseline failures are retained in logs rather
than hidden by rewriting whole-suite results. Unchanged passing tests were not
repeated after narrow corrections. Relevant log names in each profile are
`final-all-check.log`, `handoff-final-check.log`, `fixture-final-check.log`
(separate GCC/Clang integration reruns), and `view-check.log` (GCC/Clang additions).

## Native effect and failure controls

All four principal native oracles pass GCC, Clang, ASan and TSan after the handoff fix:

* Two parent turns each run local and native children: 12 real loopback TLS
  requests, four exactly-once effect files, factual parent TOOL results saved,
  all groups FREE, then bounded shutdown.
* Four alternating local/native ownership moves check durable epoch/instance,
  guard, route and activation, and physical-slot reuse with fresh view/policy
  mutation serials. Each move retains its declared 15-second budget.
* Cancellation occurs after actual native provider admission. Repeated cancel
  creates no native effect, the local sibling completes, the factual cancelled
  result is durably consumed, and the group becomes FREE within the unchanged
  five-second cleanup bound.
* The default three-second handoff failure control stops only the validated
  owned positive worker PID after source release/CAS. TIMEOUT proves no source
  thaw, no target guard/route/activation and no new effect. The fixture resumes
  or kills/reaps that managed worker and checks factual consumed cleanup.

The single-group and bounded-output rejection controls also pass. Output 4096
legitimately fails pre-STARTED reservation; 8192 admits the explicit two-child
escaped-result bounds. No guard or deadline was weakened to admit success.

## Independent corrections carried into the join

`fae1e27` fixes uniform consuming guard cleanup, custom wrapper allocation before
backend acquire, retained nonconsuming refusal, and source-freeze first-error
retention including AGAIN. Independent GCC/Clang/full-project sanitizer tests
passed before ticket closure. `1c428b2` fixes the handoff phase driver's separate
classification of synchronous consumed AGAIN; real lock/native-reply waits remain
retryable. Its independently reviewed real-file fault fixture is now registered.

Other maintained joined controls include exact clone/deep storage/rollback,
publication and two successive tool STARTED barriers, pending-RESYNC client input,
projection replay rollback/render cursor, UI lane/lease identity, actual bounded
IPC copied-send pressure, worker receipt retirement and failed reload proof
retirement. Independent leaf reviews and historical negative controls remain
in their ticket evidence. Whole O4 approval remains the different critic's task.

## Final-review startup correction

Fresh independent review found `971aeb0`: main's coordinator driver skipped all
drain steps if begin failed after spawning a managed prefix. Checked close then
refused the live owner and replaced the original startup error. `fe70b50` keeps
every successfully constructed coordinator through shutdown and cooperative drain
after begin failure, preserving the original error before consuming close.

The imported real main/library regression injects IO only on the second native
spawn. The first spawn actually executes the worker role. It checks the positive
owned PID is reaped without ownership loss, the descriptor count returns to its
baseline, the runtime pointer is consumed, and the returned error remains IO.
Fallback cleanup exists only so the broken-code negative does not leak children.
The fixture uses explicit bounded configuration and a synthetic credential with
no provider request. Worker/client startup paths have NULL-on-failure constructors
and no separate begin; worker owners are already driven to terminal, and client
close owns no effects requiring a drain. No new contract or broad change was
needed.

The registry now has 168 tests. Focused startup failure, installed headless local,
and installed empty-session controls pass 3/3 in GCC, Clang, full-project
ASan/UBSan/LSan and TSan. Exact logs: each profile's `startup-fix-build.log` and
`startup-fix-check.log`. Independent re-review of this correction remains required
before final O4 acceptance.
