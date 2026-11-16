# O implementation scaffold (gated draft)

All bodies depend on N035219b's final independent header PASS. This matrix
allocates ownership, not implementation authorization. `d5280b7` remains the
orchestrator migration umbrella; do not duplicate its missing APIs. Receipt
design300e835 is already independently passed and retained in N only. Exact child
tickets are refined when pass1 declarations freeze, then carry source, critic,
integration and finalize stages. No main merge on partial package approval.

| Group | Source lock / contract | Dependencies | Independent acceptance |
|---|---|---|---|
| O1 IPC | `libraries/cog-ipc/src/ipc.c`, private header; provenance/directed send/wipe unit fixtures | N | real SO_PEERCRED, wrong/retired peer, copied TX, EOF-retained RX before wipe, bounded queues |
| O2 core | `core.c`, `agent.c`, private headers, drive/service/agent unit fixtures | N API; integrate O1 first | run/step equivalence, finite idle/stop, fair bounded yields, event proof survives EOF, exact session borrow/return and one-shot ownership |
| O3 orchestrator | existing d5280b7: `orchestrator.c`, private header, unit-orchestrator and integration IPC/stress/aux_core fixtures; pure CGH1 codec may split after header freeze | O1 | seven actual missing functions, no synchronous handshake, auth/managed namespaces, immutable PID, SIGCHLD/exclusive reap/ECHILD, no foreign child reap, three control slots+IDs, first-error/loss metadata |
| O4 shared application integration | `cli.c`, `app_internal.h`, `app_turn.c`, `app_tools.c`, `main.c`, production CMake, all shared dispatch/glue | O1/O2; worker glue O3 | ordinary M behavior plus bounded multi-context step/drain, retained output, no competing readers, no callback reentry or task restart |
| O5 control leaf | new `control.c`, approved `control.h`, `control_receipt.c`; leaf codec fixtures | N | literal/golden/malformed/transactional frames, role+attempt binding, distinct wire/CGA/ACK state, bounded original-byte cache/watermark, cancel/lost receipt races |
| O6 child/bootstrap leaf | new executor/child/worker/delegation modules; resolved child clone in `configuration.c`; leaf fixtures | O2/O3/O4/O5 | exact local tree, real provider/tool child, parent configuration pin, intersected permission/deadline/request credit, bootstrap copy wiping, cancellation/crash/descendant uncertainty |
| O7 UI projection leaf | new projection/front-door/client modules; `app_display.c`; leaf fixtures | O4/O5, existing CGR1/CGD1 | real detached execution, DATA/GAP/CURSOR, exact committed full-state anchor, pending approval nonce, accepted-command dedup and no repeated model/effect on reconnect |
| O8 staged handoff leaf | `app_sessions.c`, `session_admission.h`, new handoff module; leaf fixtures | O3/O4/O5; final composition O6/O7 | actual source quiesce/checkpoint/release, target PREPARED/guarded READY, exact CAS/route/ACTIVATE, fresh epoch reclaim, post-CAS failure frozen, no startup input/effect replay |
| O9 original scenario join | originals03/07/09/10 in tests/scenarios, originals11/17 in app tests; real bounded process/TLS fixtures | all source groups and their critics | actual child/tool/result, owned-worker death, authenticated child messaging/fan-out, actual UI reconnect/GAP, true shared-store handoff; preserve historical component assertions separately |
| O10 package integration/finalize | additive CMake registration, evidence/contracts, installed consumer | all scoped PASS | restore all six M exclusions, all original20 joined, strict full GCC/Clang/ASan builds+scoped tests, relevant separate TSan, genuine installed relocation/dependency closure; P still owns all38/full release gate |

O1/O2 can implement in parallel after N because their source locks are disjoint;
integrate IPC before core runtime tests. O3 can start against approved O1 API,
then integrate O1 before its critic. O5 is pure codec work and can run independently
after N. O4 is ONE shared application owner; leaf groups never race its files.
O6/O7 can overlap after exact interfaces and glue hooks exist. O8 owns session
admission exclusively and must coordinate root constructor changes serially with
O4. O3 retains route/CGH implementation ownership; no app leaf edits the library.

One integration owner registers all test target snippets; no concurrent writes
to shared test CMake. O3 owns legacy07/09/10 compile migration until it explicitly
hands files to O9 for actual runtime replacement. Named constructors may allocate
within declared bounds; service cannot grow tables. Fault/allocator oracles use
the existing optimizer-safe positive-control policy. Offline synthetic TLS only;
no paid model calls are required.
