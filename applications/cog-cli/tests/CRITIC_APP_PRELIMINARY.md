# Preliminary independent shared app review

Exact joined source c319a71 (A-owned sources unchanged since260ab25).
Reviewed A delta from integrationbase d6fb056: cli/execution/app_turn/app_tools/
app_sessions/app_reload/main/configuration clone and host_client retained input.
Original D handoff excluded. Preliminary FAIL fae1e27; full O4 gate not closed.

Confirmed: cli_app_dispose destroys workspace/core/provider/config before guard
release. Built-in file_release unlinks and frees guard even if native close
reportsIO. App then returns early with a nonNULL partially destroyed owner,
dangling guard/workspace, coreNULL/configNULL. Retrying cleanup is unsafe. The
source-release path also only clears its pointer onOK, requiring the same
ownership correction; its failure must remain frozen, never pretend success.

Contract gap: public session_store.h owner_release lists onlyOK and borrower
INVALID_STATE, not per-result consuming ownership. Vtable fenced-profile comment
says errors/lifetimes match public operations; callback supplies no further rule.
file_release consumes oncloseIO; generic custom-backend wrapper retains its
wrapper onanynonOK, backend-handle lifetime unspecified. Uniform public/vtable
per-result semantics require a narrow reviewed contract amendment before fixing
callers. Do not assume a file-only workaround handles custom stores.

Regression test_app_close_guard_critic.c constructs actual configuration/app,
workspace/store/fileguard. Link wrapping invokes the real close first, then
reportsIO only inside actual owner_release. All lifecycle bodies remain real.
The test expects checked close to leave no dangling owner; current code exits1.
Diagnostic-only teardown nulls the two already-freed pointer values before
consuming remaining resources; never dereferences them or masks the failure.
Run via run_execution_tools.py to supply checkout-local durable/workspace dirs.
Link --wrap=close --wrap=cog_store_owner_release; use actual library archives.

Independent strict GCC/Clang compile and regression both reproduce. Owned-source
ClangASan/UBSan/LSan reproduces logicalfailure with no sanitizerfinding in safe
diagnostic teardown. Maintained clone, headless executor and actualtwo-tool ASK/
STARTED/publication/save fixtures PASS3/3 each GCC/Clang. Exact A-owned bodies
compiled explicitly; real immutable archived dependencies from A260ab25 snapshot
under review-o4-app/build-app-review/joined, adjacent SHA inventory. No dependency
sanitizer claim or whole native/installedrole matrix claim. Outputs/TMPDIR local.

No author tree/source modifications. Remaining app review/native fault acceptance
continues after material ownership finding is resolved. No further confirmed
findings from static transaction/barrier/role/clone/reload/input inspection.
