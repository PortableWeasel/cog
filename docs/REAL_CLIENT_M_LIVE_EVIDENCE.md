# M installed application live coding and resume smoke

Ticket `80e782e`, executed 2026-11-16 UTC after the M prerequisites integrated.
**PASS: exactly three generation requests**, each capped at512 output tokens,
with zero automatic retries and no additional live run. The unmodified machine
summary is [REAL_CLIENT_M_LIVE_RECEIPT.json](REAL_CLIENT_M_LIVE_RECEIPT.json).
This is evidence for the **installed public cog::cli application consumer**, not
a claim that the literal cog-cli executable supplied the hard paid-request cap.
Literal installed CLI behavior and the broad compiler/scenario M gate have
separate offline evidence in [REAL_CLIENT_M_OFFLINE_EVIDENCE.md](REAL_CLIENT_M_OFFLINE_EVIDENCE.md).

## Prepared and independently reviewed before payment

The consumed production checkout was exactly
`1329018c230c105f4c0f6b917f50e8a3fd0aac03`. Main subsequently advanced to
`2ab1b79c20653add0170c044c85b149db572bfcf` through test/header-smoke/docs changes;
there is no change to libraries or application implementation between those
revisions. No rebuild was needed merely to change that evidence label.

The standalone strict C11 consumer and driver are
[tests/live-client](../tests/live-client/README.md), source checkpoint
`560577576dc5a6b50c4b75b6e32f93e542f3fd3f`. Independent read-only review passed
that exact source and repeated its actual local TLS rehearsal before any live
generation. The rehearsal proved:

- Two real app/provider/net requests execute one identified command and finish
  the initial coding task; a fresh process restores the four-message history
  and completes with one request and six messages.
- A separate local unsolicited-tool response gets a durable denial with no
  effect. The app's attempted follow-up is refused before provider dispatch;
  the local server receives exactly one request for that negative case.
- Zero-budget and wrong-model/output-limit controls refuse before dispatch.
  Successful phases also test the exhausted cap directly after app teardown.
- The original credential pipe is consumed/closed during construction. The
  actual command verifies the credential environment is absent and the original
  anonymous pipe inode is absent from tool FDs. Key bytes are absent from
  captured output, disposable workspace and durable store.

The approved test-only wrapper reserves an attempt **before** calling the real
`cog_provider_send`, checks exact model/stream/max_tokens512, and admits at most
two attempts in the initial process and one in the restarted process. It does
not replace responses, change requests or fabricate outcomes. Both processes
explicitly configure max_retries0 and max_tool_rounds1; transport does not retry,
redirect, proxy or substitute a model. The live driver invokes only these two
phases and stops after a failed phase. No automatic new smoke run exists.

Receipt `refused=1` in each successful phase is the explicit **post-teardown
direct negative cap control**, using an impossible provider pointer to prove
the guard refuses before dispatch. Actual application runtime refusals were
zero; these controls sent no HTTP requests and did not consume generation.

Before the paid run, the public [official catalog](https://openrouter.ai/api/v1/models)
was checked without credentials for exact model ID and tools support. The
[official model page](https://openrouter.ai/deepseek/deepseek-v4-flash-0731)
was also checked. The announced run then used only the official base URL
`https://openrouter.ai/api/v1` and exact `deepseek/deepseek-v4-flash-0731`.

## Actual live assertions

| Phase | Admitted generation requests | Deterministic result |
|---|---:|---|
| Initial coding | 2 | One actual run_command call checked tool isolation, read main.c, changed return40 to return42, compiled with strict warnings, ran the test and wrote effect marker `applied`. Exact source/result/marker bytes and compiled program output42 passed. History has four messages, complete final assistant and one identified SUCCEEDED tool ledger. |
| Restart / RESUME | 1 | A new process restored the prior four-message conversation with its identified call/result before sending. It exposed no tools, completed one response and saved six messages. The single successful ledger and exact effect marker stayed unchanged; no old effect replay occurred. |

Both processes returned rc0/status0, had zero active net requests after the run,
closed the credential pipe and passed confidentiality checks. The model supplied
the requested tool round and successful finishes within the supported existing
profile. No parser, opaque-continuation, finish, provider or production-source
change was made to accommodate this result. Response wording is not an oracle;
raw model/log output and private credential data are not retained in evidence.

Reported cumulative usage after the first two requests was1482 input /117 output
tokens and USD0.0001012432. The final cumulative totals for **all three** requests
were1833 input /153 output tokens and USD0.0001272766. The resumed receipt includes
the earlier saved assistant usage: do not add the two cumulative cost fields.

## Genuine installed runtime and isolation

The selected c-ares build, curl build and complete Cog build were normally
installed into `/tmp/cog-live-client-original-prefix`, then that whole prefix was
renamed to `/tmp/cog-live-client-installed`. The public consumer was configured
with only the relocated CMAKE_PREFIX_PATH, built warning-free in Release and
normally installed there. Its only Cog include path is the installed include
directory; no private headers, private staging copies or build-library fallback
are used. The SDK source build used exact matching CURL_LIBRARY,
COG_NET_CURL_LIBRARY and genuine audited COG_NET_CURL_BUILD_DIR for the accepted
profile. Normal dependency installation updated install_manifest bookkeeping;
final runtime bytes remained unchanged.

Installed CMake/pkg-config/curl-config metadata contains no original prefix,
source worktree or foreign dependency/build path. The installed consumer and
CLI use `$ORIGIN/../lib`; curl and c-ares use `$ORIGIN`. Actual ldd resolution
loads both libraries from the relocated prefix with no LD_LIBRARY_PATH or
LD_PRELOAD. System OpenSSL/trust and ordinary host runtime requirements remain
as documented by the accepted dependency profile.

| Installed artifact | SHA256 |
|---|---|
| installed_live_smoke | `c69e665b747499bcc5e2d572bd3355a0dfb7a91cc3168c8809c2a0a8a53b2eea` |
| cog-cli | `be55a5a95d6577e7bd3208f6e0460290a32b6735cd5337ff59a8fcba3a62e541` |
| curl8.23.0 libcurl.so.4 | `ae1a9e77488254d744c9064972b025d8eb7210e73ea900cd5f3632995bb29d18` |
| c-ares1.34.9 libcares.so.2 | `b4d2377241c646dc2054e864e403f162d4b5969823bdd86ec5ff49ecf7e79f53` |

Only the explicit live driver read the already authorized pi OpenRouter entry,
into memory. An anonymous pipe delivered it to the configuration callback; it
was never an argument, environment value, printed/logged content or persisted
workspace/store data. Tool environment is the real workspace allowlist, and
the isolation script verified the secret channel did not reach the command.
Disposable data was removed after assertions. Only the sanitized receipt and
this evidence remain. M review/integration/closure belongs to the orchestrator;
this smoke makes no remote/headless O or final release P acceptance claim.
