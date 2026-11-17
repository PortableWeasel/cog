# Current supported release manifest

Whole O is independently accepted and merged on main. P is awaiting final
documentation review and merge; this document does not mark P complete.
This is a documentation-only record: no new runtime/model request was made.

## Exact identities

| Record | Identity / evidence |
|---|---|
| Independently tested whole-O code | `5df406946c4ba5bc5203183e4af147b867f9cc5b` |
| Independent package verdict | `5cd1d25` PASS; [complete profile record](REAL_CLIENT_WHOLE_O_PACKAGE_REVIEW.md) at `87e852fc0dba380344c711705eeb67057621cafd` |
| Approved integration merge | `b9140b68b689c0f70cedb5318347d5513f8bef79` |
| Accepted main merge | `c052b1722e42d9a8f42fc79bf35f29e4114e32a7` |
| Identical accepted integration/main tree | `e5ca810b2eaf8d86abd1dfc36a94deac459637d4` |
| Independent whole-O machine receipt | `whole-o-review-receipt.json`, SHA-256 `2bbe288476b4cd79f90a89c847a0ff09b6dc6fea3d0faee41ee3c70bb5cd1c36`; retained path and per-profile cache/log/inventory/install hashes are in the [profile record](REAL_CLIENT_WHOLE_O_PACKAGE_REVIEW.md#provenance-receipts-and-limits) |
| Coverage | [39 concrete assertion rows](REAL_CLIENT_RELEASE_COVERAGE.md): original 20,05b and R01–R18, unchanged by this status refresh |
| Runnable guide review | `bf299a9` PASS for `299e661`; [command validation](REAL_CLIENT_DOCS_VALIDATION.md) retains mock-only execution and source-audit boundaries |
| Installed public proof | `7eb47a1` PASS for `e77947c`, integrated by `f016189`; [installed receipt](REAL_CLIENT_INSTALLED_PROOF_INTEGRATION.md) |

The main merge keeps the approved whole-O tree unchanged and preserves the
user's uncommitted `AGENTS.md`/`build_user` state. The final status-documentation
change follows that code merge and requires its own independent review.

## Independent supported-profile results

All producer builds use Release, strict C11 warnings/Werror and at most two
build jobs. The full three profiles have no exclusions, disabled tests, skips
or failures. Exact commands, log hashes and actual conditional-registration
inventory are retained in the independent receipt above.

| Profile | Actual result | Seconds |
|---|---:|---:|
| GCC14.2.0 Release | 185/185 | 95.54 |
| Clang19.1.7 Release | 185/185 | 94.51 |
| Clang19.1.7 Release ASan/UBSan/LSan | 185/185 | 99.85 |
| Separate GCC14.2.0 Release TSan | 36/36 selected | 150.08 |
| Public headers: GCC/Clang C11 and G++/Clang++ C++17 | 28 headers ×4 compilers = 112 checks | — |

The TSan selection covers stop/cooperative core ownership, registry/config,
toolkit, IPC/orchestrator, child cancel/crash/mailboxes/fanout, reconnect,
migration/fencing, process-owner controls and the installed consumer. It is not
185 TSan tests. All 58 production compilation units are instrumented in the
corresponding sanitizer build. External curl/c-ares/system libraries are not;
deliberately killed processes cannot supply an exit-time leak report.

Normal full installs and two relocations pass in each profile. Both public
C11 consumers, CLI-only transitive orchestrator closure, custom fenced
release/preallocation negatives, actual relocated native READY/PID/UID/inode,
reap/retirement, two-request REAL app/tool/durable result and separate installed
MOCK headless/attach/watch roles are exercised. Positive process proof reports
cleanup true with zero infrastructure child kills. No private host-runtime
header or trust bypass is part of installed embedding.

## Admitted dependency and live evidence

| Artifact | SHA-256 |
|---|---|
| curl | `ae1a9e77488254d744c9064972b025d8eb7210e73ea900cd5f3632995bb29d18` |
| c-ares | `b4d2377241c646dc2054e864e403f162d4b5969823bdd86ec5ff49ecf7e79f53` |
| [Sanitized M live receipt](REAL_CLIENT_M_LIVE_RECEIPT.json) | `187516695653a1efaacfaa12aa76c93a41def0c7eb6b0bd410f2f5b0ce1e8d12` |

The accepted profile is Linux/POSIX with c-ares as primary resolver,
`ENABLE_ARES=ON`, `ENABLE_THREADED_RESOLVER=OFF`, fresh HTTP/1.1, no connection
pooling/HTTP2 and application-only retry ownership. Installed metadata and
actual loader paths use the relocated admitted objects without manual staging
or loader overrides. OpenSSL/libc/zlib/zstd/CA trust remain host dependencies.
Third-party dynamic memory is declared; no total dependency-memory cap exists.
These tested artifacts/profile results are not a universal platform guarantee.

[M's dated live application record](REAL_CLIENT_M_LIVE_EVIDENCE.md) remains
exactly three generations: two initial read/edit/build requests and one
fresh-process resume. It uses the installed public app, not a literal CLI
hard-cap proof or a remote-child paid/live run. No additional paid call was made
for whole-O review or this manifest; local REAL TLS scripts remain offline
protocol/effect evidence.

## Finite behavior boundaries

The [assertion map](REAL_CLIENT_RELEASE_COVERAGE.md) distinguishes canonical
same-group/retired sibling IDs from component live foreign-group refusal.
Test-only constructor admission staging keeps original frames/deadlines and
still requires all three actual children active together; an allowed
pre-STARTED AGAIN is not a production retry promise. Admitted worker failure
retains INDETERMINATE and does not authorize automatic replay/replacement or
external-effect rollback. Migration is same-host exact-checkpoint transfer,
not cross-host/live-handle migration. Installed native lifecycle and separate
public REAL app do not claim installed remote-worker REAL HTTP or composed
handoff; those are actual O9 in-tree joins. Standalone C++ header conformance
does not turn the C11 installed runtime hosts into a C++ runtime proof.

All current builds/logs/TLS scratch stay in checkout-local `build*` directories.
Dated M/F/H/O evidence, including older counts/paths/failures, remains intact.
