# Stage 14.H provider writer evidence

Delivery `055211c`; implementation `51be839`, depth admission gap `13754f7`,
independent review `01ab5f5`, integration `432bc79`, finalize `ae42487`.
Writer branch `codex/impl-provider` starts at main `e35dc9f`.
Independent review and merge remain pending; this is writer evidence.

The adapter implements the frozen generic eight-callback public base, static
OpenRouter table, strict fixed-pool JSON/SSE, bounded request serialization,
fragmented interleaved tool identity/arguments, retained transactional output,
opaque reconstruction/replay, known usage, terminal validation and sanitized
errors/cancel/deadlines. All wire work uses the actual cog-net request-local
peek/ack API. There is no retry, substitution, proxy or redirect layer.

The ordinary queue holds one item, bounded by retained_delta_count and bytes;
a constructor-owned cursor resumes final tool/opaque/usage expansion, independent
of the reserved FINISH slot. The allocator-wrapped actual-adapter suite forbids
malloc/calloc/realloc throughout send/poll/next/error/cancel. Constructor fault
injection visits every Cog reservation and verifies clean NOMEM/NULL outcomes.
Parser OOM, frame/depth/text/delta ceilings, duplicate decoded keys, malformed
UTF-8/surrogates, fragmented CRLF/multiline SSE, absent DONE, conflicting terminal
metadata, duplicate tool IDs, incomplete arguments, unknown/conflicting opaque
metadata, HTTP429, partial timeout, retained overflow+cancel and nonstream JSON
are exercised. A depth256 opaque fixture traverses repeated equal unfamiliar
metadata; ceiling257 is rejected before effects and ceiling255 returns LIMIT.

The narrow [mock amendment](REAL_CLIENT_H_MOCK_AMENDMENT.md) precedes implementation
and traces scenario15/R04 plus depth admission R03/R15. The mock now copies all
nested request data into a 128 KiB constructor arena, offers typed immutable
fixtures, retains overflow and cancelled pending items, and schedules latency
through monotonic poll without sleeping. Scripts use implementation_config.

## Local validation

Strict GCC and ASan/UBSan each pass six targeted tests:

- unit-provider and unit-provider-critic: generic dispatch/registry contracts;
- unit-openrouter: actual adapter with controlled retained transport callbacks;
- unit-mock-provider: mock independently linked without the pending CLI loop;
- integration-core-provider: migrated legacy core/text fixture;
- integration-openrouter-https: actual adapter and audited real net against a
  temporary offline TLS server, two identified tool/result/answer requests.

Strict Clang passes the five non-TLS targets. The TLS test is registered only
when the maintained transport's provenance parameters are explicitly supplied:

```sh
cmake -S . -B build-live -DCOG_ENABLE_WERROR=ON -DCMAKE_BUILD_TYPE=Debug \
  -DCOG_NET_CURL_BUILD_DIR=/tmp/cog-net-maintained/build \
  -DCOG_NET_CURL_LIBRARY=/tmp/cog-net-maintained/build/lib/libcurl.so \
  -DCURL_LIBRARY=/tmp/cog-net-maintained/build/lib/libcurl.so \
  -DCURL_INCLUDE_DIR=/tmp/cog-net-maintained/curl-8.23.0/include
cmake --build build-live --target test_provider test_provider_critic \
  test_openrouter test_mock_provider_contract test_core_provider \
  test_openrouter_https -j4
ctest --test-dir build-live -R '^(unit-provider|unit-provider-critic|unit-openrouter|unit-mock-provider|integration-core-provider|integration-openrouter-https)$' --output-on-failure
```

ASan/UBSan uses a separate directory and COG_ENABLE_ASAN=ON, leak detection,
halt_on_error and UBSan stack traces. Tests remain enabled in Release via their
explicit target flags. The legacy HTTP-only provider fake was retired because
it depended on implicit net initialization, HTTP and consume-on-poll semantics;
actual HTTPS adapter tests replace that obsolete transport coverage. No claim is
made that K's durable conversation/tool loop or all original scenarios now pass.

The provider export PUBLICly links core and net. Normal component installs stage
all result/core/ipc/obs/net/provider headers and exports. A standalone CMake
consumer uses find_package(cog-provider CONFIG), links only cog::provider, includes
both public provider and openrouter headers, explicitly initializes curl,
constructs/frees the audited net context and cleans up. It passes after prefix
relocation. The private yyjson license is installed; no vendor include/type or
build/worktree path is exported. Full application/package gate P remains pending.

## Bounded live evidence

On 2026-11-15 UTC the primary OpenRouter catalog verified exact model
`deepseek/deepseek-v4-flash-0731`, context1048576 and tools/tool_choice capability.
After strict and sanitizer fixtures passed, the actual C executable above used
the actual audited transport for exactly two generation requests, max_tokens512
each, reasoning disabled, no automatic retry or model substitution. Only
synthetic echo schema/prompt and simulated echo result were sent. This exercises
the provider tool round trip; no workspace tool executed.

| Turn | Outcome | Input/output tokens | Provider-reported USD |
|---|---:|---:|---:|
| Synthetic echo tool call | COG_OK | 301/45 | 0.000026563 |
| Synthetic result to final answer | COG_OK | 84/4 | 0.000005745 |

Total reported USD0.000032308. Credentials were loaded only into memory and
provided through stdin to the C host, never arguments/environment/logs. Only
sanitized usage/outcome lines are saved in
[live-evidence.json](../libraries/cog-provider/live-evidence.json). No raw live
headers/bodies, hidden reasoning or private source was captured. No additional
live request is needed for independent review.

## Revision 1 review corrections

Independent review of7457252 rejected three semantic cases and committed
permanent regression tests in32a6f36. This writer revision preserves those tests
and fixes their production paths without changing public headers:

- Nonstream reasoning_details is already a final ordered array. Its entries are
  copied individually after strict validation; streaming reconstruction is now
  gated on stream mode. Adjacent final text/summary entries retain their complete
  values and boundaries whether metadata differs, agrees or is absent. Explicit
  empty-array presence remains retained. Existing fragmented stream merging and
  depth256 tests continue to pass.
- Legacy mock receive combines retained text only with successful STOP. When
  cancellation follows an overflow, retry delivers nonterminal retained text,
  then exactly one ABORTED/CANCELLED completion; the following receive is idle.
- Mock cancellation snapshots include accepted retained output immediately, and
  delivery updates pending error metadata to output_accepted=true and
  replay_safe=false. Both pre-delivery and final error snapshots are asserted.

Additional exact-byte final-array tests cover absent/equal identity metadata,
summary metadata conflicts that are legitimate between separate final entries,
and an empty array. The critic's full nested request-overwrite fixture remains
passing. Strict GCC6, ASan/UBSan6 and strict Clang5 targeted tests all pass after
these corrections, including the actual offline HTTPS two-turn integration.
No additional paid request or credential access occurred. Independent re-review
is still required before H approval or merge.

## Independent acceptance and main integration

H.2 `01ab5f5` independently accepted revised tip `c170fa0` after one rejection
cycle. Regression commit `32a6f36` remains in history; the revision fixes all
three findings without changing or weakening public headers. Exact nonstream
opaque arrays retain distinct, equal and absent identities, separate summary
metadata and explicit empty presence. Legacy retained text is nonterminal before
one ABORTED completion. Cancellation error snapshots prohibit replay as soon as
retained output is accepted, including before delivery.

Independent premerge validation passed strict GCC six targets, ASan/UBSan six
targets and Clang five targets. This includes constructor fault injection,
depth256 traversal, retained typed events and actual offline TLS. No additional
paid generation or credential access occurred during review or integration.

Merge `c0b1c17` integrates the reviewed H branch into main `aa50918`, preserving
the newer J configuration implementation, application CMake linkage and I toolkit
exports. The sole conflict was additive evidence in R04: both toolkit acceptance
and the H mock fixture amendment were retained. No agent-contract or agent-code
branch was merged, and no application loop code was changed.

Postmerge strict validation passes eleven provider/mock/core/configuration
tests and all eight toolkit tests. A fresh postmerge ASan/UBSan build passes the
same six focused provider targets. The six-target set is listed under Local
validation above; the added strict checks are `unit-core`, `unit-core-service`,
`cli-options`, `cli-configuration` and `cli-configuration-memory`.

The complete strict build was attempted. Libraries and the application binary
compile; it stops at `applications/cog-cli/tests/test_15_multi_turn_tools.c:86`,
whose old positional tool initializer omits `cog_tool_t.async` under Werror.
The old application mock wiring at `applications/cog-cli/src/cli.c:791` still
passes its script through api_key instead of implementation_config. These are
pending K migration work (`78760e5`), alongside the durable conversation/tool
loop. H acceptance does not claim a full-suite, scenario15, CLI behavior or
release gate P pass.

The normal complete install succeeds. After prefix relocation, a standalone
consumer finds `cog-provider` and links only `cog::provider`, initializes the
host runtime and constructs/frees the audited net context. A second consumer
also includes the installed toolkit export with explicit Threads discovery and
whole-archive links both libraries; it runs successfully, proving private yyjson
namespaces coexist. Both licenses are installed; no yyjson public header or
worktree/build path appears in installed CMake exports. No source/export change
was needed during integration.
