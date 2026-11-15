# session_store.h Contract

`libraries/cog-core/include/cog/session_store.h` — pluggable session
persistence backend.

This document records the executable contract verified by
`tests/unit/test_session_store.c` and `tests/integration/test_store_session.c`
and implemented by `libraries/cog-core/src/session_store.c` (ticket 0efef06,
Stage 12.2d). It intentionally does **not** expand the public API; it
documents what the header declares and pins the implementation's
interpretation of every ambiguous point so the Critic can review the choices.

## Status

Implemented. Guard: `COG_SESSION_STORE_H`. Includes `<cog/result.h>`,
`<cog/session.h>`, `<stdbool.h>`, `<stddef.h>`, `<stdint.h>`.

## Public types

| Type | Kind | Notes |
|------|------|-------|
| `cog_store_t` | opaque struct | Created by `cog_store_new`, destroyed by `cog_store_free`. |
| `cog_store_save_fn` | function pointer | `cog_result_t (*)(void *, const cog_session_t *)`. |
| `cog_store_load_fn` | function pointer | `cog_result_t (*)(void *, const char *, cog_session_t **)`. |
| `cog_store_append_fn` | function pointer | `cog_result_t (*)(void *, const cog_session_t *, const cog_session_node_t *)`. |
| `cog_store_delete_fn` | function pointer | `cog_result_t (*)(void *, const char *)`. |
| `cog_store_list_fn` | function pointer | `cog_result_t (*)(void *, char ***, size_t *)`. |
| `cog_store_exists_fn` | function pointer | `bool (*)(void *, const char *)`. |
| `cog_store_vtable_t` | struct | `name` + the six operation pointers. |
| `cog_store_config_t` | struct | `backend`, `base_dir`, `max_sessions`, `auto_save`, `compress`. |

## Constants / globals

| Symbol | Meaning |
|--------|---------|
| `cog_store_file_backend` | `extern const` file backend vtable (default). |
| `cog_store_null_backend` | `extern const` no-op backend vtable. |

Both are plain `static const`-style vtable instances defined in
`session_store.c`; there is no backend registry and no global mutable state.

## Functions and error codes

Every function returns `cog_result_t` except `cog_store_free` (void),
`cog_store_exists` (bool), and `cog_store_auto_save` (bool). Codes marked
**(ext)** are fail-closed extensions not listed in the header's `@return`
block but consistent with `cog/result.h`; they are pinned by the tests.

| Function | Success | Errors |
|----------|---------|--------|
| `cog_store_new` | `COG_OK` | `NULL_PTR` (out NULL), `NOMEM`, `INVALID_ARG` (ext: backend vtable has a NULL operation) |
| `cog_store_free` | — (void) | NULL is a no-op |
| `cog_store_save` | `COG_OK` | `NULL_PTR` (store/session NULL), `INVALID_ARG` (ext: unsafe id), `OVERFLOW` (ext: id too long), `IO`, `NOMEM` |
| `cog_store_load` | `COG_OK` | `NULL_PTR` (store/id/out NULL), `NOT_FOUND`, `INVALID_ARG` (ext), `OVERFLOW` (ext), `IO` (malformed/corrupt), `NOMEM` |
| `cog_store_append` | `COG_OK` | `NULL_PTR`, `INVALID_STATE` (ext: node not in session), `INVALID_ARG` (ext), `OVERFLOW` (ext), `IO`, `NOMEM` |
| `cog_store_delete` | `COG_OK` | `NULL_PTR`, `NOT_FOUND`, `INVALID_ARG` (ext), `OVERFLOW` (ext), `IO` |
| `cog_store_list` | `COG_OK` | `NULL_PTR` (store/out_ids/out_count NULL), `IO`, `NOMEM` |
| `cog_store_exists` | bool | false for NULL store/id, unsafe/over-long id, or absent session |
| `cog_store_auto_save` | bool | false for NULL store; otherwise the configured `auto_save` flag |

`COG_ERROR_NOMEM` is not injectable through this API (no allocation-failure
hook), so it is documented but not exercised by the tests.

### Null backend semantics

The null backend's own documentation ("all operations return COG_OK but do
nothing") takes precedence over the generic `load` doc. Concretely:

- `save` / `append` / `delete` return `COG_OK` and touch nothing.
- `list` returns `COG_OK` with `*out_ids = NULL` and `*out_count = 0`.
- `exists` returns `false`.
- `load` returns `COG_OK` and sets `*out = NULL` (nothing was loaded; the
  caller never receives uninitialised output).
- Argument validation still applies: NULL store/session/id/out return
  `COG_ERROR_NULL_PTR` / `false` exactly as for the file backend.

### File backend on-disk layout

One directory per session under `base_dir`:

```
<base_dir>/<session_id>/meta.json
<base_dir>/<session_id>/nodes.jsonl
<base_dir>/<session_id>/index.bin
```

- `meta.json` — `{"version":1,"id":"<b64>","parent_id":"<b64>"|null,
  "max_nodes":N,"node_count":N,"next_node_id":N,"active_node_id":N}`.
  `id` is base64 of the session id and must equal the directory name.
- `nodes.jsonl` — append-only, one JSON object per node in DFS pre-order
  (every parent precedes its children; sibling order is preserved). Each
  record carries `node_id`, `parent_id`, `role`, and base64-encoded string
  payloads (`content`, `tool_name`, `tool_args`, `tool_result`,
  `raw_request`, `raw_response`, `model`, `provider`), plus `chunk_count`,
  `timing{...}` and `cost{...}`. Base64 preserves embedded NUL bytes and
  byte lengths exactly.
- `index.bin` — advisory `node_id -> byte-offset` cache: 8-byte magic
  `COGIDX01`, a native-order `uint64_t` count, then `count` pairs of
  `(uint64_t node_id, uint64_t offset)` sorted by node id. It is maintained
  by `save` (full rebuild) and `append` (in-order extension when cheap);
  `load` treats `nodes.jsonl` as the source of truth, so a missing or
  corrupt `index.bin` is **non-fatal** and is regenerated on the next full
  save. Native byte order is documented (the cache is rebuildable, so it is
  not a portable interchange format).

`save` writes each file to a `.tmp` sibling and renames it into place
(three independent atomic renames, not one atomic transaction). A crash
mid-save can leave an inconsistent store that `load` rejects with
`COG_ERROR_IO` (fail closed); re-saving repairs it.

### Append semantics

`append(session, node)` persists exactly one node:

- If the session directory does not exist, the store **bootstraps**: it
  creates the directory, writes a fresh `meta.json` (node_count 1) and a
  one-entry `index.bin`, and writes the node as a standalone record. A
  non-root node is snapshotted with `parent_id = 0` so the single-record
  store loads as a one-node session (its in-memory parent is not part of
  the persisted store yet).
- If the session exists, the node record is appended to `nodes.jsonl` and
  `meta.json` is rewritten with the incremented record count and the
  appended node as the active node.
- Appending a node whose id is already in the file produces a duplicate
  record; `load` rejects duplicate node ids with `COG_ERROR_IO` (caller
  misuse, fail closed).
- `append` does not prune `max_sessions` (that is a `save`-time
  maintenance operation).

### Delete failure safety

Before unlinking anything, `delete_session_dir` enumerates the complete
session directory and rejects unknown entries. It also rejects a directory
masquerading as one of the six known store files (`meta.json`,
`nodes.jsonl`, `index.bin`, and their `.tmp` siblings). An expected
content-related failure therefore returns `COG_ERROR_IO` with the session
and its metadata intact: `exists` remains true, a repeated delete reports
`IO` rather than `NOT_FOUND`, and removing the obstruction allows delete
to succeed.

The store is single-threaded by contract. That contract does not make the
preflight-plus-unlink sequence atomic against unrelated external
filesystem mutation; such concurrent mutation is unsupported and may
still produce an `IO` failure after a partial unlink. No filesystem-wide
or cross-process locking is attempted.

### max_sessions pruning

After a successful `save`, if `max_sessions > 0` and the store holds more
sessions, the oldest session directories (by directory mtime) are deleted
until the count fits. The just-saved session is never pruned. `max_sessions
== 0` means unlimited. `append` does not prune.

### compress / auto_save

Both flags are accepted and retained by the store.

- `compress` is **not** honoured by the file backend: the documented
  on-disk layout is plain JSONL + index, and the store does not pretend to
  compress. A `compress = true` store writes the same plain files and
  round-trips identically.
- `auto_save` is recorded by this module and exposed read-only through
  `cog_store_auto_save()` (false for a NULL store). The store itself
  performs **no implicit writes**: the *trigger* lives in the core. Per
  `cog/core.h`, when a store is configured and `cog_store_auto_save()` is
  true, `core.c` saves the session once after each successfully handled
  `COG_EVENT_LLM_COMPLETE` exchange (see the core CONTRACT). This module
  only records and reports the flag.

### Path safety

Every session id is validated before it is used as a directory name:

- empty id, `"."`, `".."`, ids containing `/` or `\`, and ids containing
  control bytes (0x00–0x1F, 0x7F) are rejected with `COG_ERROR_INVALID_ARG`
  (`exists` returns `false`);
- ids longer than 128 bytes are rejected with `COG_ERROR_OVERFLOW`
  (`exists` returns `false`);
- a valid id may otherwise contain any printable bytes (e.g.
  `under_score.dash-1`).

A hostile id can never escape `base_dir`.

## Ownership

- `cog_store_new` copies `config->base_dir`; the store owns its copy and
  releases it in `cog_store_free`. The caller retains ownership of the
  `config` struct and of any custom backend vtable.
- `cog_store_load` returns a newly allocated session owned by the caller
  (freed with `cog_session_free`). On failure `*out` is left untouched.
- `cog_store_list` returns a caller-owned array of caller-owned id strings;
  the caller frees each string and the array. An empty store returns
  `*out_ids = NULL`, `*out_count = 0`.
- The store never owns sessions or nodes; `save`/`append` only read them.
- A core borrows its store (`cog_core_config_t.store`); `cog_core_free`
  does not free it (verified by `tests/integration/test_store_session.c`).

## Invariants

- Node ids and parent ids are persisted exactly and restored exactly; a
  loaded session continues its id sequence from the stored `next_node_id`.
- The loaded session restores the stored active node, node cap
  (`max_nodes`), parent-session id, timing, cost, chunk counts, and the raw
  provider fields (`raw_request`, `raw_response`, `model`, `provider`).
  The last two groups have no public setter, so the store restores them
  through the private `session_internal.h` layout (the same single
  definition session.c and core.c use); no public API is expanded.
- `content` is length-prefixed and may contain embedded NUL bytes; the
  base64 wire format preserves them. `tool_args` / `tool_result` /
  `tool_name` are NUL-terminated by construction in session.c, so their
  wire representation is lossless through the public API.
- A loaded session whose stored `node_count` exceeds its stored
  `max_nodes` is rebuilt with temporary headroom and then restored to the
  stored cap (the cap is a soft limit; the tree is never truncated on
  load).
- `load` fails closed on: missing session (`NOT_FOUND`), missing
  `meta.json` (`NOT_FOUND`), malformed/truncated `meta.json` or
  `nodes.jsonl` (`IO`), a version mismatch (`IO`), a meta id that does not
  match the directory name (`IO`), a record count that does not match
  `meta.json` (`IO`), an unknown role, invalid base64, duplicate node ids,
  a missing root or more than one root, a parent that does not precede its
  child, an invalid `active_node_id`, or a `next_node_id` that is not
  strictly greater than every node id (`IO`).
- `list` returns only subdirectories of `base_dir` that contain a
  `meta.json`; junk files and directories without `meta.json` are ignored.
  Results are sorted lexicographically.
- A missing `base_dir` behaves as an empty store for `list` (OK, count 0)
  and `exists` (false); `load`/`delete` return `NOT_FOUND`. A `save` to a
  `base_dir` whose parent does not exist returns `COG_ERROR_IO`.

## Threading

A store is owned by exactly one core instance and is not thread-safe; all
access happens on the thread that runs `cog_core_run`.

## Complexity

- `save` is O(nodes) with an iterative DFS (no recursion-depth risk) plus
  an O(n log n) index sort (insertion sort on a near-ordered list).
- `load` is O(nodes) with a backward linear scan for parent lookup
  (O(n) typical, O(n²) worst case for adversarial id orders).
- `list`/`exists`/`delete` are O(sessions) directory scans.
- `append` is O(1) amortized (one record write + meta rewrite).