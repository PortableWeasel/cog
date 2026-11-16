# Consuming local cleanup errors in handoff

Ticket 1c428b2: local source release is synchronous. Its consuming AGAIN must
retain the first error and freeze/abort the handoff immediately, rather than
become generic phase waiting until TIMEOUT. Completed local disposal similarly
uses factual `local_done` to distinguish final cleanup errors from drain progress.
Native exchange waits and target guard acquisition remain retryable.

Imported independent regression 09b9706 and extended it with four actual-file
consuming-disposal cases: normal cleanup and abort-target cleanup, each with OK
or prior PROTOCOL primary. Assert exact retained error, app NULL, retained slot,
no second release, and eventual factual done. The file guard really closes and
is freed before the test shim converts its IO result to the contract-valid AGAIN.

The actual handoff leaf additionally exercises an unanswered remote source
release and a real independently held target store lock. Both return AGAIN
without abort/freeze or effects. Releasing the lock permits normal stage guard,
activation, and cleanup.

Author validation: both fixtures PASS strict GCC and Clang, and Clang ASan/UBSan
with leak detection and all project archives instrumented. Local build logs:
`build-release-review` (fault fixture), `build-r` (leaf; shortened to fit Unix
socket path bounds). Explicit current A/core sources plus immutable actual
GCC archive snapshot260ab25; sanitizer archives snapshot5e24284, externalcurl
uninstrumented. No stubs, no shared CMake edits, no whole native gate claim.

Registration snippet for the integration owner: build
`test_handoff_release_again_critic.c` with the same app/test includes and real
libraries as `test_app_close_guard_critic`; link `--wrap=cog_store_owner_release`
and `--wrap=close`; run using `run_execution_tools.py EXECUTABLE BUILD_DIR`.
The fixture includes handoff.c directly, so do not separately add that source.
