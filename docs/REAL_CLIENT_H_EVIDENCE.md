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
