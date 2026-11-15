# Bounded network observation delivery

Implementation `1a2b9c8`, approved contract `7096360` under gate `d32fafd`.
The managed implementation branch incorporates oracle fixes from main `081e1f5`.
Production implementation is confined to cog-net; public headers, application,
provider and obs bridge implementations are unchanged. Independent acceptance
and integration remain required. This does not close release gate P or the
subsequent application bridge delivery `b64ee7c`.

## Source boundary

Constructor-owned private request/context records now hold the borrowed callback
and user identity, sticky observer result, checked per-request numeric sequence,
body totals/reported totals and fixed wire outcome. No observer queue, dynamic
registry, new constructor allocation or runtime Cog allocation is introduced.
The private layout header is not installed or exposed as a public dependency.

OPEN follows out_id publication. CONNECTED is emitted from the existing actual
curl prerequisite callback, after connection/TLS readiness and before HTTP
transmission, rather than inferred from submission or response data. This is the
[documented prerequisite boundary](https://curl.se/libcurl/c/CURLOPT_PREREQFUNCTION.html).
Inbound body totals advance only after the complete callback input fits and is
accepted transactionally into the body ring. Paused retries do not count twice.
Uploaded entity bytes come from
[CURLINFO_SIZE_UPLOAD_T](https://curl.se/libcurl/c/CURLINFO_SIZE_UPLOAD_T.html),
not the submitted body size; the pinned curl implementation reads its uploaded
progress counter. Headers, TLS and framing do not enter these body totals.

Progress is coalesced per service call. A fixed wire outcome flushes remaining
progress before one terminal even when retained response-terminal publication
still awaits quantum. Response inactivity emits STALLED then FAILED; connect
failure/timeout and backpressure do not invent STALLED. A captured completion
wins over later cancel/release. Fatal driver errors leave requests owned/live
without a fabricated terminal; explicit cleanup supplies the real final outcome.
The existing no-replay prerequisite guard and transport settings are preserved.

Any callback error disables the context feed and latches its first result,
including AGAIN or NOMEM. Sequence/body-counter exhaustion latches LIMIT;
unavailable, negative or regressing upload accounting latches IO. These changes
never modify transport outcomes, retained bytes or unrelated shared requests.
Peek/ack/status emit nothing. Active cancel/release can flush progress plus one
terminal; free destroys the domain without callbacks. Observer/user lifetimes
remain host-owned. The approved no-reentry rule is a caller precondition;
debug assertions check it without adding undocumented ordinary API error codes.

## Executable evidence

The writer fixture uses an ephemeral trusted loopback TLS service, a local UDP
NXDOMAIN/blackhole resolver for the admitted c-ares implementation and a reserved
unlistened TCP port. Tests cover:

- Successful POST readiness, exact uploaded/accepted body totals, HTTP429 with
  transport CLOSED/OK, zero submitted-versus-uploaded confusion before service,
  nonzero progress deltas and final progress/terminal total equality.
- Coalescing bounds per request/service, transactional body-ring pause/retry,
  backpressure time exclusion, and completion captured before response-terminal
  publication winning over cancel and release.
- Actual DNS, TCP refusal and TLS hostname failure; connect timeout without
  STALLED; response inactivity with STALLED then FAILED; capacity LIMIT.
- Errors at every callback kind, including OPEN AGAIN/NOMEM, sticky first-error
  status, disabled feed, constructor OOM without callbacks, NULL user and
  disabled-observer ignored user, silent free, repeated cancel/release, and
  distinct live contexts both owning local request ID1.
- A failed feed on a shared net preserving both a retained backpressured GET and
  an independently completed once-only POST. Callback/accounting failures cannot
  rewrite a completed POST result or cause replay. The writer server observes
  exactly seven intended POST effects across its entire fixture.
- No URL/header/credential/body text in copied numeric observations; no direct
  Cog malloc/calloc/realloc during request/service; positive allocation controls;
  private numeric-boundary injection for sequence/body-counter LIMIT and
  unavailable/negative/regressing upload-accounting IO, with real outcomes kept.

The different critic's [deterministic driver-failure fixture](REAL_CLIENT_NET_OBSERVATION_CRITIC.md)
was incorporated without production changes (`44107f6`, original `5384d98`).
It proves progress and retained-view stability after a real POST plus an injected
fatal driver error, followed by one explicit ABORTED and exactly one server effect.

Final writer validation on 2026-11-15: strict full target builds pass under GCC
Release/O3, Clang Release/O3 and Clang Release/O3 with ASan/UBSan. Each profile
passes all **16/16** focused tests: the thirteen net suites (including both
observer fixtures), unit-openrouter, unit-provider-preflight-critic and
integration-openrouter-https. Sanitizers use detect_leaks and halt_on_error.
All use `/tmp/cog-net-maintained/build`, its matching admitted c-ares-only shared
curl and installed headers. Diff whitespace checks pass. No paid/live request or
full release validation claim is included; the next independent verdict must
assess this complete checkpoint.

The final critic-driven fixture correction removes a late `/stall` body write:
HTTP/1.1 now leaves the five declared remaining bytes permanently withheld until
client cancellation. This avoids a delayed-client race with body completion.
The changed observer fixture passes again under all three optimized/sanitized
profiles; production source and the previously passing sixteen-test set are
unchanged.

## Independent acceptance and main integration

Different critic review passed cycle 1 on complete writer checkpoint `bd02941`.
Production source stayed at `1f829ec` and the public header exactly matched
approved `7096360`; the critic made no production changes. Independent full
strict GCC Release, Clang Release and Clang Release ASan/UBSan/LSan builds each
passed the same 16/16 focused tests. The critic regression and final review are
recorded in [independent evidence](REAL_CLIENT_NET_OBSERVATION_CRITIC.md).

Root-authorized no-fast-forward merge
`1484675f06f31de95f8bb379d4d10e868a5a8aa2` integrated reviewed `fb0faff`
(writer checkpoint plus critic acceptance and approved DESIGN decision) into
main `081e1f5` without conflicts. Fresh main
`build-review-net-observation` completed a full strict Clang Release build
without warnings and passed all 16/16 net/consumer checks. The admitted
c-ares-only curl build/library/include provenance and test scope match the
independent runs above.

The approved design/header/source commits are retained as ancestors of main.
The original critic test `5384d98` and incorporated `44107f6` have the identical
stable patch ID `d5f71de2ec7398d0451974c4b9a752e241951f33`; its original review
branch is also retained. The managed net implementation, review and completed
observability-design worktrees are archived after these checks. Existing
`build_user/` and all R12 worktrees are preserved; the R12 writer was notified
to retain both approved DESIGN decisions on its later integration.

This closes only net primitive `1a2b9c8` and its scoped validation child.
Application bridge/timing `b64ee7c` remains pending after R12 `af6956a`;
M and P remain open. No paid/live request was made.
