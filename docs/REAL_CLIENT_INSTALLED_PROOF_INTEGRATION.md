# Approved installed-proof integration

Independent review `7eb47a1` approved exact installed extension
`e77947c5982f128890faa3582b1501119fc0d8e0`. Integration `f016189` merged it
conflict-free into O9 integration `d16f3c8c29fdffee56a32af9fc74f52ceb49aad7`
using no-ff merge `58beaf62debfae100ed6436e9af609fde32a5d00`.

The merged tree before this evidence-only addition is
`2cddf1a5385baa8fb18a203c5028f9a590ea6ea7`. All twelve incoming files are under
`tests/external-consumer`; their subtree exactly matches the independently
reviewed source. The production diff against accepted O4 `e8c155f` is empty.
No application private header was copied or promoted to a public API.

## Postmerge execution

The existing checkout-local `build-o4-merge` GCC14.2.0 Release/Werror producer
was incrementally reconfigured and built with two jobs. Its admitted dependency
builds and relative install policy remain unchanged; no dependency rebuild or
new producer tree was needed. Shared dependency installation bookkeeping was
free before the full run. O4 and O9 logs/cache snapshots remain retained.

```sh
export TMPDIR="$PWD/build-o4-merge/tmp"
mapfile -t dependency_args < build-dependencies/cmake-args.txt
cmake -S . -B build-o4-merge -DCMAKE_BUILD_TYPE=Release \
  -DCOG_ENABLE_WERROR=ON -DCMAKE_C_COMPILER=gcc "${dependency_args[@]}"
cmake --build build-o4-merge --parallel 2
ctest --test-dir build-o4-merge --show-only=json-v1
ctest --test-dir build-o4-merge --output-on-failure --parallel 2
```

Strict all-target build passed without warnings. Full CTest passed **185/185**
in **91.27 seconds**, with zero exclusions, disabled tests, skips or failures.
The exact inventory adds only `unit-installed-process-owner` to O9's184; no
test was removed. Both external consumers execute within the existing
`integration-installed-embedder`, so they do not add another CTest registration.
The extended installed fixture passed in4.64s. Native O9 scenarios retain their
resource serialization and all unchanged historical/component assertions.

Actual normal installs and two relocations passed both standalone C consumers
and the installed CLI. The CLI-only target links only `cog::cli` and checks its
transitive orchestrator export. The public fenced custom backend checks normal
and recovery preallocation failures before callback, borrowed/callback
`INVALID_STATE` retention, consuming IO/AGAIN, real reacquisition and no repeated
release. Public CGW1/CGH1 round trips and fenced advisory owner routes execute.
The actual relocated native worker supplies kernel PID/UID READY proof and the
installed executable inode, then asynchronously shuts down and is reaped and
retired. Its minimal environment excludes the synthetic credential/loader
canaries. The positive supervisor receipt has status0, cleanup true and **zero
infrastructure kills**; success/exit17/SIGABRT/timeout escaped-descendant controls
also pass.

The separate public REAL app makes exactly two loopback TLS requests, executes
one command effect, checks the correlated TOOL result and durable SUCCEEDED
ledger, then exercises cooperative core stop/end. Installed headless MOCK,
watch and attach verify an explicit second turn, stable session/replay identity,
detachment and signal drain. Original nine-callback provider, independent custom
stores/net contexts, explicit host curl lifecycle, metadata/path negatives and
relative loader checks remain intact.

The report records these unchanged admitted runtime digests:

- curl: `ae1a9e77488254d744c9064972b025d8eb7210e73ea900cd5f3632995bb29d18`
- c-ares: `b4d2377241c646dc2054e864e403f162d4b5969823bdd86ec5ff49ecf7e79f53`

Actual loader paths for both C hosts and CLI resolve curl/c-ares inside the final
relocated prefix, without loader overrides or manual staging. Platform OpenSSL,
libc, zlib/zstd and CA dependencies retain their declared host scope.

| Retained log / receipt | SHA-256 |
|---|---|
| `build-o4-merge/configure-installed-proof.log` | `d3ac723b293e30439116edd55caffc0af476eb783c0c0015c1ca357446f2f8d8` |
| `build-o4-merge/build-installed-proof.log` | `b534cfa2f9f6f464e3dc5c9fb68e86c7e62b7c90a18374cfbcae3a5dc4fefa1f` |
| `build-o4-merge/ctest-installed-proof.log` | `4ef7626212cc7d35c8c7e83b33e4870a3b47a89f98b557130a8ec4040988790f` |
| `build-o4-merge/test-inventory-installed-proof.json` | `f7964ece76e8654a9d423b0925c2aaed4178a6c55b0febbd08e5f3aaca57ca5b` |
| `build-o4-merge/installed-embedder-report.json` | `0cfb260b59fdee34b61d6f47f642572325089c037b44328bcdbf5feab0311bed` |

`build-o4-merge/installed-proof-integration-receipt.json` records exact source,
merge/tree identities, inventory difference, profile, test counts and hashes.
Writer and independent reviewer receipts/logs remain in their retained worktrees.
The independent Clang19 external-consumer run used the trusted GCC producer;
it is not a fresh full Clang or sanitizer producer result.

## Scope

Installed public worker lifecycle and the distinct public REAL TLS app are the
external boundary. Remote-worker REAL HTTP and actual composed handoff remain
O9's in-tree proof; the installed CLI trust template has no local-CA input.
No trust bypass, private host-runtime embedding, paid call or new public API was
introduced. This closes only installed integration `f016189`. Whole-O package
review, all38/full supported-release P and any main merge remain separate.
Main's user `AGENTS.md` and `build_user/` are untouched.
