# O gated per-file tickets

Every source depends on N035219b. IDs below are actual tix records; no dispatch
is authorized until independent whole N PASS. Group finalization is O-branch
integration only; root owns eventual main merge.

| Group | Parent | Review | Integration | Finalize |
|---|---|---|---|---|
|O1 authenticated bounded IPC|e7c11c7|cdc91f7|3fd8cc6|5e0617d|
|O2 cooperative core and exact child lease|99c42ec|8ffb926|65c50c6|565225d|
|O3 nonblocking authenticated orchestrator|d5280b7|235ca88|048a4d0|904975c|
|O5 pure private application codecs|7e03cac|67e6da9|6bb6cc1|69451cf|
|O6 resolved profiles and host role parsing|f2b389c|4f39316|fcc0f17|51c386d|
|O8 canonical projection and attach client|9d57bf5|1672a1c|f3fd542|6e67e63|
|O4 actual executor coordinator delegation and handoff composition|50fad6e|df377bf|7041c2d|ae7f775|
|O9 replace historical substitutes with actual joined scenarios|f63652b|4b5a102|8537bc9|2187146|

## O1

Prerequisites: N only. Shared source lock: O1.

| Source | Ticket |
|---|---|
|`libraries/cog-ipc/src/ipc.c`|7b5592d|

## O2

Prerequisites: O1. Shared source lock: O2.

| Source | Ticket |
|---|---|
|`libraries/cog-core/src/core.c`|e55f22c|
|`libraries/cog-core/src/agent.c`|959efdc|

## O3

Prerequisites: O1, O2. Shared source lock: O3.

| Source | Ticket |
|---|---|
|`libraries/cog-orchestrator/src/orchestrator_wire.c`|9ccb5d5|
|`libraries/cog-orchestrator/src/orchestrator.c`|dff0484|

## O5

Prerequisites: N only. Shared source lock: O5.

| Source | Ticket |
|---|---|
|`applications/cog-cli/src/control.c`|c5b7bd3|
|`applications/cog-cli/src/control_receipt.c`|272eb0b|
|`applications/cog-cli/src/projection_codec.c`|0482897|
|`applications/cog-cli/src/child_context.c`|a08e2b0|

## O6

Prerequisites: N only. Shared source lock: O6.

| Source | Ticket |
|---|---|
|`applications/cog-cli/src/configuration.c`|3575897|
|`applications/cog-cli/src/host_options.c`|a2d466a|

## O8

Prerequisites: O1, O5, O6. Shared source lock: O8.

| Source | Ticket |
|---|---|
|`applications/cog-cli/src/projection.c`|a89efb1|
|`applications/cog-cli/src/host_client.c`|c4105bf|

## O4

Prerequisites: O1, O2, O3, O5, O6, O8. Shared source lock: O4.

Private layout/helper header stage BEFORE bodies: 860d5cc.

| Source | Ticket |
|---|---|
|`applications/cog-cli/src/cli.c`|8f94d02|
|`applications/cog-cli/src/execution.c`|b89a8fc|
|`applications/cog-cli/src/app_turn.c`|e75a155|
|`applications/cog-cli/src/app_tools.c`|c9b1548|
|`applications/cog-cli/src/app_sessions.c`|b5d2304|
|`applications/cog-cli/src/app_reload.c`|0876b96|
|`applications/cog-cli/src/host_runtime.c`|725e4aa|
|`applications/cog-cli/src/delegation.c`|1110ab9|
|`applications/cog-cli/src/worker_host.c`|8927216|
|`applications/cog-cli/src/handoff.c`|4b68739|
|`applications/cog-cli/src/main.c`|27508d8|

## O9

Prerequisites: O4. Shared source lock: O9.

| Source | Ticket |
|---|---|
|`tests/scenarios/test_03_subagent_lifecycle.c`|bf50c23|
|`tests/scenarios/test_07_aux_crash.c`|5a34f65|
|`tests/scenarios/test_09_subagent_messaging.c`|c4b9fa0|
|`tests/scenarios/test_10_fan_out_fan_in.c`|7733fb4|
|`applications/cog-cli/tests/test_11_ui_reconnect.c`|e6c7e2a|
|`applications/cog-cli/tests/test_17_session_migration.c`|64a76b4|

Whole O package acceptance/finalize: **1cc6b4f**.

Pure wire/profile/projection groups can run disjointly after N and their listed
primitives. O4 owns shared app integration and all additive test-CMake snippets;
its per-file children are one coordinated source lane until private layouts are
frozen. A leaf never edits app_internal.h/main/CMake behind that owner. O9 begins
actual scenario replacement only after source ownership handoff. All6 M runtime
exclusions remain explicit until whole O acceptance; P remains all38/full release.
