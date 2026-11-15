# Core-local agent lifecycle integration

Package `bf7a450` passed independent review `4cde520` revision 1/4 at
`e04df57`. It was integrated into main with non-fast-forward merge
`e0fca98a9f3203fab5452b1466c6fa99f3ad5790`. The approved branch already contained
main `fad0337`, so no rebase or conflict resolution was needed. Existing H, I
and J history was preserved. The pre-existing untracked `build_user/` directory
was left untouched.

[Independent review](REAL_CLIENT_LOCAL_AGENT_REVIEW.md) records the three
header/trace/consistency passes, strict GCC/Clang builds, ASan/UBSan and TSan,
allocation bounds and public-path regressions. Header-design commit `bec6a00`
is patch-equivalent to integrated `f531420`; the six scoped header/scenario
files are identical between those commits.

## Postmerge checks

Fresh builds on merged main used the repository strict warning policy and
`COG_ENABLE_WERROR=ON`:

- GCC 14.2.0: session, session-codec, session-allocations, event, state, core,
  agent and core-service passed (8/8).
- Clang 19.1.7: core, agent and core-service passed (3/3).
- Standalone public headers compiled under GCC/Clang C11 and G++/Clang++17:
  26 headers each, 104 successful objects.
- Original scenario targets 03/07/09/10 compiled with strict GCC. Both C
  compilers compiled these four targets during independent review.
- Whitespace checks passed.

Build directories are `build-agent-integration` and
`build-agent-integration-clang`. Focused runtime checks can be reproduced with
`ctest --test-dir <build> -R '^unit-(agent|core|core-service)$' --output-on-failure`.

The four original scenario runtimes were also rerun on merged main. All four
remain failing in deferred integration paths:

| Scenario | Remaining observed runtime failures |
|---|---|
| 03 | CLI application creation |
| 07 | Orchestrator session registration/readiness and CLI application creation |
| 09 | IPC registration/routing/message delivery and CLI application creation |
| 10 | Orchestrator health/placement, cross-peer cancellation and CLI application creation |

These results do not establish completion of K, N, O, M or the full release
gate. The existing full-default strict build failure at scenario 15's old tool
initializer remains outside this package, as recorded in the independent review.
No real network or credentials were used.

## Scoped completion and remaining ownership

The constructor-bounded legacy agent migration `b569276` now passes with the
implemented local lifecycle. Implementation `0629c76`, independent review
`4cde520`, integration `dbcad81`, header gap `86f7a3c` and package `bf7a450`
are complete for their core-local scope. The public primitives now present on
main are atomic leased SPAWN/CANCEL admission, copied resolved configuration,
correlated bounded update retention, copy/ack backpressure, terminal result
reads, release and the CGA1 codec. Stage O uses these primitives; authentication,
transport forwarding, placement, process drain/reap, reconnect and application
runtime integration remain its work. K and M runtime gates stay open.

The finished managed `test-agent-migration` and `agent-intent-contracts`
worktrees are archived through the Codex worktree lifecycle. Their commits are
preserved; implementation/review commits are reachable from main, and the
original equivalent header-design commit remains in the archived history.
