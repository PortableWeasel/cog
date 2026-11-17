# Runnable documentation validation

Documentation task `71bcd03` starts from accepted integration `2723c36`.
Checkpoint `08dfcea` supplies the runnable guide/current status and the original
20 plus 05b/R01–R18 map; the followup adds formatting, this receipt and validation.
Only documentation changes. No source/API, user `AGENTS.md`, dated evidence,
dependency, paid-model request or release-gate status is changed.

## Static checks

- Read the actual application option/default table, host-role option parser,
  terminal/recovery commands, public custom-store ownership contract and current
  accepted evidence. REAL is the zero/default backend; explicit mock never
  supplies a paid/live-provider result.
- All 12 README Bash blocks pass `bash -n`. The admitted-profile variables are
  documented inputs from the authenticated recipe, not newly built artifacts.
- All relative Markdown targets in the six changed guide/design/map documents
  resolve. The assertion map has exactly 39 ordered IDs: 01–05,05b,06–20,R01–R18.
  Every explicitly named CTest resolves in the accepted 185-case inventory.
- `git diff --check` passes. Historical M exceptions/counts/paths are retained in
  their dated evidence; current status does not falsely keep L/M/N or
  orchestrator migration `d5280b7` pending.

## Actual lightweight commands

The existing accepted CLI at
`integrate-headless/build-o4-merge/applications/cog-cli/cog-cli` was used without
a producer rebuild. Paths were translated to isolated checkout-local
`docs-release/build-doc-check/tmp` scratch; COG configuration variables and the
OpenRouter key were absent, with isolated XDG config. No native worker or REAL
model command was run by this documentation check.

| Case | Executed behavior / assertion |
|---|---|
| Explicit mock prompt | `--backend mock --no-store --prompt hello`, exit 0. |
| Explicit mock chunks/queue | `--chunks first,second --queue 'next turn'` with mock/no-store, exit 0. |
| Durable NEW | Mock `--store ... --session demo --prompt hello`, exit 0. |
| Offline list | `--store ... --sessions` with default REAL but no model/key; exit 0 and actual `demo` ID. |
| RESUME | Mock `--resume demo --prompt 'next turn'`, exit 0. |
| Terminal inspection | Mock resumed interactive `/history`, `/tools`, `/usage`, `/quit`; exit 0 and both earlier user messages preserved. |
| FORK | Mock `--fork demo --from-node 2 --prompt alternative` at the observed completed safe point, exit 0. README uses the general NODE selected from `/history`. |
| Headless/attach/watch | Actual `--headless --backend mock --session headless-demo`, physical `--attach`/`--watch`, explicit second input, same session/generation and increased replacement sequence, four rows, `/quit` detach and SIGTERM drain. All three processes exit 0; coordinator emits no conversation directly. Workers 0. |

Retained local receipts/logs:

- `build-doc-check/mock-command-receipt.json` records the exact executable hash
  and arguments, with separate per-command stdout/stderr.
- `build-doc-check/headless-command-receipt.json` and
  `doc-headless.*`, `doc-attach.*`, `doc-watch.*` record the actual local UI join.

REAL, native, recovery and install examples were checked against the actual
option/contract/source boundaries and accepted runtime evidence, not executed
as new paid work or a new heavyweight fixture. Recovery deliberately requires
an actually unresolved/fenced session; the clean demonstration cannot be used
as a positive reconciliation fixture. The complete installed proof and custom
fenced callback examples already have their independently accepted receipt.

## Scope

This is documentation implementation evidence only. Independent documentation
review must approve the final text. Whole-O profile review and P retain their
own source/inventory/compiler/sanitizer/runtime receipts. In-flight profile
failures or test-only admission-staging corrections are not rewritten as a
completed whole-O pass here.
