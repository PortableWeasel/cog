# Independent net observation checks

Ticket `1a2b9c8`, final writer checkpoint `bd02941`, reviewed separately in
managed `review-net-observation`. Production source remains exactly `1f829ec`,
and the public header remains exactly approved `7096360`. No production edits
were made by the critic. Final source review verdict: **PASS, cycle 1**.

The independent regression uses an offline, ephemeral trusted TLS service. The
service accepts one complete POST body as its effect, writes five response bytes,
then waits for the test's completion signal before ending its connection. A
linker wrapper injects a fatal `curl_multi_timeout` error only after those bytes
have entered Cog retention. This is a deterministic post-progress driver-error
boundary, rather than a sleep-based race with EOF.

Required observations and ownership checks:

- Service returns IO while the owned request remains active and observer status
  remains OK; real connection readiness and accepted/uploaded progress are
  delivered, without an invented wire terminal.
- Retained metadata is acknowledged to select the held body view. The held
  `hello` bytes remain stable across cancellation.
- Explicit cancellation emits one FAILED/ABORTED with HTTP200 and the exact
  five inbound bytes and actual uploaded POST-body total. A repeated cancel,
  release and context destruction emit no additional terminal/callback.
- The service receives exactly one `critic-effect` POST. No retry is performed
  to compensate for the driver failure or cleanup.

Strict Release GCC and Clang-ASan/UBSan/LSan builds each pass all 13 net tests,
including this independent regression. Strict Clang passes the existing 12 net
tests and the corrected independent regression. The first draft of the probe
incorrectly assumed the first response item was body rather than retained
header metadata; it failed that harness assertion, was corrected to acknowledge
metadata, and then passed. This was not a production defect or timing waiver.
The production source remains at the reviewed checkpoint throughout these runs.

The final complete writer checkpoint was then checked independently: full
strict Release target builds and all **16/16** focused net/consumer tests pass
under GCC, Clang and Clang ASan/UBSan/LSan. These include all thirteen net suites,
unit-openrouter, unit-provider-preflight-critic and integration-openrouter-https.
The final observer fixture permanently withholds the remaining `/stall` bytes
until client cancellation, removing the late-body scheduling race. The added
writer cases cover shared-net failure isolation, context identities, every
callback kind, DNS/TCP/TLS/connect-timeout distinctions and body capacity.

Source review confirms constructor-owned per-context state, actual curl
readiness, accepted inbound and actual upload accounting, coalesced progress,
fixed completion before response-terminal publication, one cleanup terminal,
sticky observation errors independent of transport results, caller no-reentry
preconditions and no new runtime Cog allocation or payload-bearing callback.
Review requests improved tests/documentation; no production revision was needed.

All builds use the admitted c-ares-only maintained curl build at
`/tmp/cog-net-maintained/build`, its matching shared libcurl and installed
headers. Sanitizer tests enable leak detection and halt on error. No paid/live
request, app/provider implementation change or release-gate closure is included.
