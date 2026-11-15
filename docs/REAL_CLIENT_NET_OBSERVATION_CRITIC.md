# Independent net observation checks

Ticket `1a2b9c8`, initial source checkpoint `1f829ec`, reviewed separately in
managed `review-net-observation`. Final approval waits for the writer's complete
test/evidence checkpoint. No production edits were made by the critic.

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

All builds use the admitted c-ares-only maintained curl build at
`/tmp/cog-net-maintained/build`, its matching shared libcurl and installed
headers. Sanitizer tests enable leak detection and halt on error. No paid/live
request, app/provider implementation change or release-gate closure is included.
