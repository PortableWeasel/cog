# config.h Contract

`libraries/cog-config/include/cog/config.h` — a hierarchical configuration
tree with path-based get/set/delete/list, candidate transactions with
atomic commit and rollback, and per-subtree validators.

This document records the executable contract verified by
`tests/unit/test_config.c` and implemented by
`libraries/cog-config/src/config.c` (ticket df4d0fa). It intentionally
does **not** expand the public API; it only documents what the header
already declares, pinning the exact semantics the implementation must
hold.

Coverage table: every claim below is exercised by `tests/unit/test_config.c`
(the top-of-file comment maps test groups to the sections below).

## Status

**Implemented** (`libraries/cog-config/src/config.c`). The tree is an
opaque struct owning a running node tree, a fixed-capacity validator
table, and every transaction begun on it. There is no global mutable
state and no dynamic registration: the validator table is a
compile-time-sized array embedded in the tree.

Guard: `COG_CONFIG_H`. Includes `<cog/result.h>`, `<stdbool.h>`,
`<stddef.h>`. Compiles standalone under the project's strict warning set
with GCC 14 and Clang 19.

## Public types

| Type | Kind | Notes |
|------|------|-------|
| `cog_config_tree_t` | `struct cog_config_tree` (opaque) | Running configuration; definition in `src/config.c`. |
| `cog_config_txn_t` | `struct cog_config_transaction` (opaque) | Candidate snapshot; definition in `src/config.c`. |
| `cog_config_validator_fn` | function pointer | `cog_result_t (*)(const cog_config_txn_t *, const char *)`. |

Values have exactly one type, `const char *` (owned copies stored in
the candidate). There is no typed-value API; the nearest "wrong type"
case — reading a node that exists but carries no value — is pinned to
`COG_ERROR_NOT_FOUND` (see reads below).

## Path grammar

A path is one or more dot-separated segments, each non-empty
(`providers.openai.base-url`). Rules, enforced identically by every
operation:

- `""` is invalid for `cog_config_set`, `cog_config_get`, and
  `cog_config_delete` → `COG_ERROR_INVALID_ARG`. It is valid for
  `cog_config_list` (lists the tree root's children) and for validator
  registration (whole-tree validator).
- Leading/trailing dots and empty interior segments (`.`, `a.`, `.a`,
  `a..b`, `a.b.`) → `COG_ERROR_INVALID_ARG` on every operation.
- A node may hold both a value and children (`set` on a container is
  legal); neither operation invalidates the other.

## Lifecycle

| Function | Contract |
|----------|----------|
| `cog_config_tree_new(&out)` | `COG_OK` + empty tree; `COG_ERROR_NULL_PTR` if `out==NULL`; `COG_ERROR_NOMEM` on allocation failure. `*out` is NULLed before any failure. |
| `cog_config_tree_free(tree)` | Frees the running tree, all string copies, every validator prefix, and every transaction (active **or** finalized) ever begun on the tree. NULL is a no-op. |
| `cog_config_begin(tree, &txn)` | Deep-copies the running tree into a fresh candidate. `COG_ERROR_NULL_PTR` on NULL args; `COG_ERROR_NOMEM` on failure. Multiple concurrent transactions per tree are allowed; each is an independent snapshot. |
| `cog_config_commit(txn)` | See Transactions. |
| `cog_config_rollback(txn)` | Discards the candidate; running tree unchanged; transaction finalized. NULL is a no-op; rolling back an already-finalized transaction is a no-op. |

## Transactions

Transactions follow the PoC (`pocs/config-tree`) candidate model:
`begin` snapshots the running tree, `set`/`delete` mutate only the
candidate, and the running tree is untouched until commit.

| Claim | Behavior |
|-------|----------|
| Copy-on-commit | Commit validates then swaps the candidate into the running tree by pointer; nothing in the running tree is ever mutated in place, so a failed or partial commit can never expose a half-applied configuration. |
| Atomic visibility | A reader snapshot taken before a commit continues to see the pre-commit tree (hot-reload isolation); a snapshot begun after the commit sees the new tree. Verified by `test_commit_visibility` and `test_snapshot_isolation_stress`. |
| No-op commit | A commit with no staged edits runs validators and succeeds (`COG_OK`); the running tree is unchanged. There is no declared "clean commit skips validators" carve-out, so validators always run against the candidate. Verified by `test_noop_commit`. |
| Validator rejection | Returns `COG_ERROR_INVALID_ARG`. The staged edits are discarded and the candidate is re-derived as a clean copy of the unchanged running tree; the transaction remains usable, and a retry after new `set` calls is possible. Verified by `test_validator_rejects`. |
| Finalization | A successful commit or a rollback finalizes the transaction: the candidate ceases to exist. Afterwards `commit`, `set`, and `delete` return `COG_ERROR_INVALID_STATE`; `get` and `list` return `COG_ERROR_NOT_FOUND`; `rollback` is a no-op. Verified by `test_commit_finalizes`, `test_rollback_discards`. |
| Handle safety | A finalized transaction struct is retained (but inert) by its tree until `cog_config_tree_free` and is never recycled, so a stale handle can never alias a different transaction and the INVALID_STATE path is deterministic. |

## Path-based access

### `cog_config_get(txn, path, &out_value)` — returns `const char *`

- `COG_ERROR_NULL_PTR`: `txn`, `path`, or `out_value` NULL.
- `COG_ERROR_INVALID_ARG`: empty/malformed path.
- `COG_ERROR_NOT_FOUND`: path missing; path resolves to a node carrying
  no value (container read); or the transaction is already finalized.
- `COG_OK`: `*out_value` points into the candidate (borrowed; valid
  until the transaction is finalized). It is NULLed before any failure.

### `cog_config_set(txn, path, value)`

- `COG_ERROR_NULL_PTR`: `txn`, `path`, or `value` NULL (empty string
  `""` is a legal value).
- `COG_ERROR_INVALID_ARG`: empty/malformed path.
- `COG_ERROR_INVALID_STATE`: transaction already finalized.
- `COG_OK`: intermediate nodes created as needed; the value is copied;
  overwriting replaces the previous copy. On allocation failure the
  nodes created along the failed branch are removed again — a failing
  `set` never leaves partial nodes (`COG_ERROR_NOMEM`).

### `cog_config_delete(txn, path)`

- `COG_ERROR_NULL_PTR`: `txn` or `path` NULL.
- `COG_ERROR_INVALID_ARG`: empty/malformed path.
- `COG_ERROR_INVALID_STATE`: transaction already finalized.
- `COG_ERROR_NOT_FOUND`: path absent.
- `COG_OK`: removes the subtree and prunes empty, valueless ancestors
  (deleting the last leaf under a subtree removes the subtree).
  Verified by `test_delete`.

### `cog_config_list(txn, path, out_keys, max_keys, &out_count)`

- `""` lists the tree root's children; any other path must exist.
- `COG_ERROR_NULL_PTR`: `txn`, `path`, or `out_count` NULL; also when
  `max_keys > 0` and `out_keys` is NULL.
- `COG_ERROR_INVALID_ARG`: malformed non-empty path.
- `COG_ERROR_NOT_FOUND`: path absent, or transaction finalized.
- `COG_ERROR_OVERFLOW`: `max_keys > 0` and the node has more children
  than `max_keys`; `*out_count` receives the real count; nothing is
  truncated (fail loudly).
- Count-only mode: `max_keys == 0` allows `out_keys == NULL` and
  returns the count without writing keys.
- `COG_OK`: `out_keys` receives up to `max_keys` borrowed pointers to
  child names, in the tree's child order (insertion order; deletions
  shift, re-inserts append). Verified by `test_list`.

## Validators

`cog_config_register_validator(tree, prefix, fn)`:

- `COG_ERROR_NULL_PTR`: `tree`, `prefix`, or `fn` NULL.
- `COG_ERROR_INVALID_ARG`: malformed (non-empty) prefix.
- `COG_ERROR_OVERFLOW`: table full — capacity is fixed at 16 slots per
  tree, embedded in the struct; registration never grows at runtime.
- `COG_ERROR_NOMEM`: prefix copy failed.
- Duplicate prefixes are allowed; registration order is preserved.

At commit, validators run in registration order:

- A validator fires iff its prefix exists in the candidate (the empty
  prefix `""` is the whole-tree root and always exists, i.e. a
  whole-tree validator fires on every commit). A prefix removed by
  `delete`, or never present, is skipped — verified by
  `test_validator_absent_prefix`.
- The validator receives the transaction (candidate) and the prefix; it
  inspects the candidate through the same public read API (e.g.
  `cog_config_get`). Verified by `test_validators_basic`
  (`base_url_validator`).
- The first non-`COG_OK` verdict aborts the commit (see Transactions).
- Duplicate registration of the same prefix runs the validator once per
  slot.

## Ownership model

| Object | Allocated by | Freed by |
|--------|--------------|----------|
| Tree struct + running tree + validator prefixes | `cog_config_tree_new` / `cog_config_register_validator` | `cog_config_tree_free` |
| Transaction struct + candidate tree | `cog_config_begin` | Retained by the tree; released by `cog_config_tree_free` (candidate released earlier by commit/rollback) |
| Value/name/prefix strings | copied on set / register | with their owning node / slot |

No allocation is hidden: every allocation has a matching release reachable
through the public lifecycle above.

## Threading, zero-global-state, and reload

- A tree is not thread-safe, per the header. No locking is performed.
- No global mutable state and no dynamic registration: the only static
  storage is `static const` (none is needed; the validator table is
  per-tree and fixed).
- **Reload / change detection / diff**: config.h declares no diff,
  listener, or reload API — nothing is implemented beyond the declared
  surface. Per scenario 08 (`docs/scenarios/08-config-reload.md`),
  hot reload is: reload source → `begin` → `set`/`delete` → `commit`,
  with the application signalling `COG_EVENT_CONFIG_CHANGED`
  (cog/event.h, kind 16) after a successful commit. This unit provides
  the two properties the reload relies on: atomic visibility of the new
  running tree, and snapshot isolation for readers that began before the
  commit ("in-flight requests are unaffected").
- Rollback restores the pre-transaction running tree (the transaction is
  simply never applied). There is no multi-commit revision stack and no
  `rollback()` of a *previous committed* state: config.h declares
  none, so none is implemented.

## Error-code coverage

All documented error codes are exercised by the tests except
`COG_ERROR_NOMEM`, which is not injectable through the public API (the
library exposes no allocation hooks); `cog_config_commit`'s NOMEM
degraded path (re-derive after validator rejection fails) is documented
above and noted in the header. ASan/UBSan/LSan runs of
`tests/unit/test_config.c` (including the stress tests:
500 commit/rollback cycles, 32 concurrent snapshots, 100-wave mixed
interleave, 80-level deep paths) are clean.

## Files

| File | Purpose |
|------|---------|
| `include/cog/config.h` | Public contract (the truth). |
| `src/config.c` | Node tree, path resolution, transactions, validators. |
| `tests/unit/test_config.c` | Executable contract test (this module's own; wired via `libraries/cog-config/CMakeLists.txt` — integration ticket dd2f55e owns the shared tests/unit registration). |
| `contracts/CONTRACT.md` | This document. |
