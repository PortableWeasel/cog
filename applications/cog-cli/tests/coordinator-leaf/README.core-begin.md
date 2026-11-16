# Pre-spawn control begin failure

Ticket971aeb0 follow-up: at e94f8d8, failed control drive begin set shutdown=true
directly. Subsequent request_shutdown returned early, leaving the constructed
zero-child orchestrator outside its shutdown lifecycle; host step could never
declare native_done. No child/process error is needed to trigger this leak.

c4f882f keeps begun and control_drive_ended ownership flags and the original
error, then invokes the single existing request_shutdown path. It neither
forges native_done nor ends a drive that was never acquired. Earlier actual
partial-spawn/main cleanup remains unchanged.

The critic's test5b07cce is retained unchanged. Actual REAL configuration/net/
store/core/orchestrator constructors are used, only the first drive_begin is
injected IO. The original source fails convergence after32steps, then its
explicit test-only orchestrator fallback disposes actual ownership before
reporting failure. Corrected code converges without fallback, returns firstIO
through every step and consuming close, and has no used worker reservation.

Strict GCC and Clang direct checks pass at c4f882f. Same-test original-source
negative control reproduces converged=0; corrected builds report converged=1.
Logs/commands: fix-host-core-begin/build-begin-check/check.log and check.py.
Actual archives: review-o4-final/build-review-gcc; exact shipped dynamic curl
profile from integrate-headless/build-dependencies. No heavy rebuild or source
acceptance claim. Independent final critic owns instrumented profile coverage.
