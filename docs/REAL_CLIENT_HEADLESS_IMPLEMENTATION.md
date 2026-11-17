# O implementation strategy (N gated)

The [actual per-file tickets](REAL_CLIENT_HEADLESS_TICKETS.md) are the current
source/review/integration/finalize ownership map. They supersede the provisional
read-only inventory; every body depends on independent whole N035219b PASS.
Existing d5280b7 remains the orchestrator migration umbrella, with exact native
wire and nonblocking source children, not a duplicate task.

| Group | Source ownership | Dependencies / parallel boundary |
|---|---|---|
| O1 IPC | ipc.c and owned private state/socket fixtures | N; independent of pure codecs/profiles |
| O2 core | core.c + agent.c, one owner | O1; shared run/step/proof/lease state |
| O3 orchestrator | orchestrator_wire.c + orchestrator.c, native/unit/integration fixtures | O1/O2; restore actual nonblocking production, not fixture-only AGAIN |
| O5 pure codecs | control.c,control_receipt.c,projection_codec.c,child_context.c | N; no I/O/effect/role authority, exact standalone focused checks |
| O6 resolved profiles/options | configuration.c + host_options.c | N; no source reread/callback transfer, no app CMake edits |
| O8 projection/client | projection.c + host_client.c | O1/O5/O6; bounded ring/render, no executor ownership |
| O4 cohesive app composition | cli/execution/turn/tools/sessions/reload,host_runtime/delegation/worker_host/handoff/main | O1/O2/O3/O5/O6/O8; ONE shared source lane until internal layout/helper header stage freezes. All per-file children exist, but no circular partial-runtime reviews/stubs. |
| O9 joined original scenarios | exact03/07/09/10/11/17 paths in ticket map | approved O4; preserve component assertions, use actual production execution |

O4's explicit planning child commits private implementation state/helper
interfaces BEFORE any dependent bodies; public/behavior gaps go back to N, not a
workaround. This is implementation layout, not permission for another architecture.
Leaf writers can split disjoint files only after that ownership handoff, and never
race app_internal.h, main.c or shared app CMake. The static table/role/control
contracts are already fixed by N. App display's public CGD1 implementation and
completed ipc_replay.c stay unchanged unless a genuine traced defect requires it.

Constructor allocations are explicit/named; no service table growth. O4 adds
cog::orchestrator to CLI's actual transitive package/link graph (acyclic app→orch→
core/IPC), installs/exports existing target names and preserves admitted dependency
profile/relative RPATH/caller policy. Pure codec targets may use focused builds
before the whole source composition links; they cannot substitute runtime stubs.
One integration owner registers additive test CMake snippets; assertion execution
and optimizer-safe allocation/fault oracles remain intact.

Actual O acceptance joins local/worker prompts/tools, authenticated sibling
mailboxes, fan-out/cancel, detached UI replay and staged same-host handoff. All six
M exclusions must pass after actual runtime wiring; all20 plus R13/R14 run against
production paths. GCC/Clang strict all-target builds, scoped ASan/UBSan/LSan,
meaningful separate TSan and genuine installed/relocated consumer/CLI/worker
checks are mandatory. Offline synthetic REAL TLS only; no paid requests needed.
P remains responsible for all38/full supported-release acceptance.
