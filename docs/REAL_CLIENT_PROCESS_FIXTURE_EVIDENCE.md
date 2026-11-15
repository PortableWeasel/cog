# Process fixture readiness correction

Ticket `b4c9ca3` passed independent review revision 1/4 at approved tip
`6912c4d4712bae69b66fa575049d2274b04363da` (writer `c1cb6d0` plus the critic's
deadline oracle). Merge `c5edcf6b6fea9857221d1aaa364d3e77f2e47a8b` integrated it
into main `42c5e15` without conflicts. Current application and assertion-policy
history was preserved; the existing untracked `build_user/` was not changed.

Only `tests/unit/test_workspace.c` and
`tests/unit/test_toolkit_review_faults.c` change runtime fixture behavior.
Production sources, public headers and CMake policy are unchanged.

The process fixtures publish readiness after installing traps and writing PIDs.
The orphaned-grandchild case waits for the independently supervised shell to be
reaped before cancellation/timeout polling. Readiness has a three-second bound;
drain/result loops retain a five-second bound. An already-expired public
execution clock is first serviced after readiness, so startup scheduling cannot
prevent the test from observing its owned grandchild PID.

A normal accepted TERM may produce the trapped exit status 0 or a legitimate
deadline-driven KILL status 137 under load. Accepted-send counts, transient
control retry limits, one attempt per poll, one execution, retained result
copies, fsync-failure preservation, descendant disappearance and ECHILD checks
remain asserted. The critic's added oracle requires every KILL attempt to occur
at or after the original public cancel/timeout-poll timestamp plus its grace.
Zero-grace and persistent-backpressure cases retain their stronger KILL rules.

Independent premerge fresh strict GCC 14, Clang 19 and Clang ASan/UBSan builds
passed both focused targets. Each profile ran the two tests ten times while
all three profiles ran concurrently with `-j2`, pinned to one allowed CPU:
60/60 executions passed with no sanitizer diagnostic. This supplements the
writer's reported 180 focused passes.

Postmerge fresh **Release + Werror** builds of both focused targets passed with
GCC 14 and Clang 19 ASan/UBSan. Generated test flags put `-UNDEBUG` after
`-DNDEBUG`; the production toolkit archive retains its existing Release flags.
Both profiles then ran simultaneously, each with two tests repeated ten times
and `-j2`, pinned to CPU 0: **40/40 executions passed**. ASan used
`detect_leaks=1:halt_on_error=1`; UBSan used
`halt_on_error=1:print_stacktrace=1`.

Builds/logs are `build-fixture-integration-gcc` and
`build-fixture-integration-asan`, each with `fixture-integration-load.log`.
Reproduce the focused selection with
`ctest --test-dir <build> -R '^unit-(workspace|toolkit-review-faults)$' --repeat until-fail:10 -j2 --output-on-failure`.
The clean managed writer worktree is archived and its commits remain reachable
from main. Whitespace checks pass.

This closes the fixture correction only. Full supported-profile suites after
integration, all-scenario runtime acceptance and release gate P remain open;
these focused checks do not replace them. The old branch's scenario-15 build
failure was historical K work, not a current-main failure established here.
