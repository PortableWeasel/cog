# Whole-O supported-profile package review

Independent review `5cd1d25` **PASS** for code
`5df406946c4ba5bc5203183e4af147b867f9cc5b`, based on approved integration
`2723c36ac913924dbe31b8adec0ebf14cb095bde` plus the two test files in
correction `b994884`. Libraries, application implementation and public headers
remain unchanged from accepted O4 `e8c155f`.

The separately approved documentation snapshot is `299e661`, integrated as
`20d36cbd`. Its [current release assertion map](REAL_CLIENT_RELEASE_COVERAGE.md)
is the coverage reference: twenty originals, shutdown regression05b and R01–R18
are39 assertion rows, not39 independently named CTests. This review does not
close whole-O finalize `1cc6b4f`, release P or authorize a main merge.

## Final independent matrix

Every producer used Release, C11, `COG_ENABLE_WERROR=ON` and the project strict
warnings, with at most two build jobs. Full GCC/Clang/ASan runs selected all185
registered tests: zero exclusions, disabled tests, skips or failures. Profiles
and native/install runs were serialized. No sanitizer suppression was used.

| Producer | Final result | Wall seconds | Test log SHA-256 |
|---|---:|---:|---|
| GCC14.2.0 Release |185/185|95.54|`3cd40058c4dbc0824c801ed634b60d5ae1b61fc66fa1847752a642e889114b23`|
| Clang19.1.7 Release |185/185|94.51|`5fcc6dcd609dc8b955f42f45d570924b03f3a015f8a04284ac13c01c8f50e307`|
| Clang19.1.7 Release ASan/UBSan/LSan |185/185|99.85|`60d17356c92c75f73851c4083f0eff3efd8ebcd2277cd441faf855df434605d5`|
| Separate GCC14.2.0 Release TSan |36/36 selected|150.08|`174ee689f97e21cf7093e38e0e6f7aeb7688db703cdc0b6e4aeea726349edd37`|

Exact log paths:

- GCC: `/home/user/ws-2611/cog-worktrees/review-o9/build-review-release/whole-o-test-full.log`
- Clang: `/home/user/ws-2611/cog-worktrees/review-o9/build-whole-clang/test-full-corrected.log`
- ASan: `/home/user/ws-2611/cog-worktrees/review-o4-final/build-review-asan/whole-o-test-full.log`
- TSan: `/home/user/ws-2611/cog-worktrees/review-core-o2/build-review-tsan/whole-o-test-selected.log`

All58 production compilation units were checked for strict warnings and the
selected instrumentation. ASan uses `-fsanitize=address,undefined` and
`-fno-omit-frame-pointer`, with
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. The separate TSan build uses
`-fsanitize=thread -fno-omit-frame-pointer` and
`TSAN_OPTIONS=halt_on_error=1`. No profile combines ASan and TSan.

The36-case TSan selection exercises core stop/cooperative driving, registry,
configuration transactions/cloning, asynchronous toolkit lifetime/cancellation,
IPC ownership, orchestrator wire/spawn/reap/stress, shutdown05/05b, reload08,
actual child cancellation, both native crash modes, sibling mailbox pressure,
local plus two-native fanout/cancel, UI reconnect, four migrations/post-CAS
failure, process-supervisor negatives and the installed consumer. The complete
selection is retained at
`/home/user/ws-2611/cog-worktrees/review-core-o2/build-review-tsan/whole-o-selected-tests.json`.
This is a meaningful separate scope, not a claim of185 TSan executions.

## Failure found and resolved

The first combined Clang run at `2723c36` passed184/185; natural positive
scenario10 observed a genuine FAILED child before STARTED. An observational
fixture copy added only `accepted_outcome` to its failure print. A bounded
reproduction captured an authentic AGAIN outcome in round2 with no requested
cancellation. The new run did not pinpoint a new syscall origin; prior O9
syscall evidence establishes store-domain-lock contention as an allowed source
of consuming constructor refusal. No production retry follows from that error.

Correction `5df4069` delays only an unaccepted actual PROFILE transport copy.
For each fresh group, the next constructor waits for its predecessor's current
STARTED/RUNNING state and genuine held post-tool TLS request. Original frame
pointer/length/wire and operation deadline remain retained. Before releasing
any terminal response, all three actual executors must be active together on
one local and two distinct authenticated native placements. Original reverse
completion, submission-ordered results, once-only effects, fresh repeated IDs,
retirement, cancellation and admission-limit assertions remain.

Writer stress passed14/14 executions across Clang and ASan with two-CPU process
pressure: five positive two-group runs plus unchanged cancel/limit per profile.
The final full matrices above independently execute the corrected case. The
initial failure was not discarded or counted as passing evidence.

Retained initial log:
`/home/user/ws-2611/cog-worktrees/review-o9/build-whole-clang/test-full.log`,
SHA-256 `48a4b77784bc292909d917fe047a700503c72b446c3bfdf842340e5bbaa5797f`.
The authentic outcome diagnostic is
`/home/user/ws-2611/cog-worktrees/review-o9/build-whole-clang/fanout-authentic-outcome-16.log`,
SHA-256 `892bcbe92c57f2346391892916f7006f8d4cd10bbc40e34f8a451acc49dc4c72`.

## Contracts, coverage and installed boundary

All28 public headers compile individually with strict warnings/Werror under
GCC/Clang C11 and G++/Clang++ C++17:112 successful combinations. Header source
hashes and individual outcomes are in
`/home/user/ws-2611/cog-worktrees/review-o9/build-whole-clang/public-headers/inventory.json`,
SHA-256 `bfa6db0696e5fbd0f99b55d4292551d1eb2534db031da1f68ff3fe8f7fbf4093`.
Generated commands confirm Release assertions remain active with `-UNDEBUG`
after `-DNDEBUG`, including the static shared fixture. The full suites also
pass assertion-side-effect, intentional-failure, registration and allocator
controls. These are standalone C++ header checks; installed runtime hosts are C11.

The [coverage map](REAL_CLIENT_RELEASE_COVERAGE.md) names exact sources,
registered cases and actual oracles. Its mappings were checked against the185
entry inventory and source:

- All original01–20 and05b execute. The six historical assertion sets remain
  separate components; the actual03/07/09/10/11/17 joins do not use manually
  completed agents or a fixture peer as their runtime authority.
- R13 joins actual sockets, replay GAP/atomic replacement, retained ASK, genuine
  lost-final command retransmission, expired lease and no repeated effect,
  supplemented by bounded replay/projection/client components.
- R14 joins authenticated owned native attempts, ordered updates/results,
  semantic acknowledgments, exact worker death/reap/uncertainty, sibling tools
  and distributed workload/cancellation. Canonical same-group/retired-ID tests
  remain distinct from the actual mailbox component's foreign-group refusal.
- Remote R16 is covered by actual native cancellation/drain and failure modes,
  alongside05/05b, stop/tool/signal tests and guarded migration failure. It makes
  no automatic replay/failover or external-effect rollback claim.
- Every former M exclusion is enabled and passes in the three full matrices:
  `unit-orchestrator`, `integration-orchestrator-ipc`,
  `integration-orchestrator-stress`, and original07/09/10. All six names also
  occur in the TSan selection.

[Installed public integration](REAL_CLIENT_INSTALLED_PROOF_INTEGRATION.md)
retains its reviewed public-only boundary. Each final profile independently
passes normal installs/two relocations, both public C consumers, CLI-only
transitive orchestrator export, custom fenced backend/reservation negatives,
CGW1/CGH1/routes, actual installed worker kernel PID/UID/executable identity,
reap/retirement and explicit installed headless/watch/attach roles. Each report
records two actual loopback REAL requests, one unchanged command effect, a
successful durable ledger, cleanup true and zero infrastructure child kills
for the positive public process proof.

The installed native lifecycle and distinct public REAL application do not
claim installed remote-worker REAL HTTP or composed handoff. Those remain
actual O9 in-tree joins; no private runtime header or trust bypass was added.
The [dated M live record](REAL_CLIENT_M_LIVE_EVIDENCE.md) remains exactly three
generations, two initial and one resumed, through the installed public app.
It is neither a literal CLI hard-cap proof nor a remote-child live run. This
review made no new paid/live request and does not rewrite that historical record.

## Provenance, receipts and limits

Every profile used the admitted checkout-local dependency arguments from
`/home/user/ws-2611/cog-worktrees/integrate-headless/build-dependencies/cmake-args.txt`.
Actual normally installed/loaded dependency digests match:

- curl: `ae1a9e77488254d744c9064972b025d8eb7210e73ea900cd5f3632995bb29d18`
- c-ares: `b4d2377241c646dc2054e864e403f162d4b5969823bdd86ec5ff49ecf7e79f53`

The resolver has `ENABLE_ARES=ON` and `ENABLE_THREADED_RESOLVER=OFF`. Installed
metadata/loader checks use the final relocated prefix without manual staging or
loader overrides. External curl/c-ares and system libraries are uninstrumented;
compiler sanitizers cover the58 Cog production units. Deliberately killed
processes cannot supply exit-time leak reports. Actual owned-process absence,
reap and uncertainty checks remain separate from sanitizer exit reporting.

Builds, logs, temporary TLS keys, workspaces and TMPDIR stayed in checkout-local
`build*` directories. Old profile logs/cache receipts and failing diagnostics
were preserved. No test exclusion or deadline increase was used to pass a case.

The machine receipt contains exact source/tree identities, profile paths/cache
and build-log hashes, final test logs/inventories, header checks, installed
reports, dependency digests, sanitizer options and retained diagnostics:

`/home/user/ws-2611/cog-worktrees/review-o9/build-whole-clang/whole-o-review-receipt.json`

SHA-256: `2bbe288476b4cd79f90a89c847a0ff09b6dc6fea3d0faee41ee3c70bb5cd1c36`.

This is the supported-profile independent package verdict only. Whole-O
finalization, all38/full supported-release P and any main merge remain separate.
