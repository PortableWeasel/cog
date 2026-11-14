# PoC 4 — Hierarchical config commit/rollback

This proof-of-concept validates the **Configuration** functional block of
`docs/DESIGN.md`: a VyOS-style hierarchical configuration model with
path-based access, candidate staging, per-subtree validation, and atomic
commit/rollback semantics.

## What it proves

- `set(path, value)` / `delete(path)` on dotted paths such as
  `providers.openai.base-url`.
- Staged changes live in a **candidate** tree and are invisible to readers
  until a successful commit (review-before-apply).
- **Validators registered per subtree** run against the candidate at
  commit time.
- **Atomic commit** — a validator rejection or an allocation failure leaves
  the running tree bit-for-bit untouched.
- **Rollback** restores the previous known-good running tree.

## Data structure

A configuration is a tree of nodes:

```c
struct cog_cfg_node {
    struct cog_cfg_node *parent;
    char                *name;    /* NULL only for the tree root   */
    char                *value;   /* NULL when the node has none   */
    struct cog_cfg_node **children;
    size_t               child_count;
    size_t               child_cap;
};
```

Any node may carry both a value and children. Paths are resolved by
splitting on `.` and walking child arrays with `memcmp` (a single
`O(depth)` linear scan per level). A `cog_cfg_t` owns two trees plus one rollback slot, and a dirty flag:

| Field       | Role                                                          |
|-------------|---------------------------------------------------------------|
| `running`   | What the rest of the system reads (`cog_cfg_get`).            |
| `candidate` | **Session copy** of the running tree plus staged `set`/`delete` edits; reviewed via `cog_cfg_show_candidate`. |
| `saved`     | The previous running tree — the rollback point.               |
| `dirty`     | Object-level flag: candidate differs from running.            |

Each node also carries a `dirty` bit, propagated to ancestors on every
`set`/`delete`. It scopes validator firing to *changed* subtrees and makes
“nothing staged” detectable without comparing trees.

### Memory ownership model

- `cog_cfg_t` is **opaque and caller-allocated**: the caller provides the
  storage (`malloc(cog_cfg_sizeof())`, sized via the public size query
  since the struct is opaque) and passes it to `cog_cfg_init()`;
  `cog_cfg_fini()` frees every internal allocation (nodes, values,
  validator records) and the caller frees the struct itself. There are no
  hidden allocations and no global state. (This matches the design
  blueprint's “every context is caller-allocated, every allocation is
  visible” rule; an alternative that also satisfies it — a public struct
  with a private fixed-size blob — was rejected because it leaks sizing
  into the ABI.)
- `cog_cfg_set` duplicates the value string into the candidate tree; the
  caller keeps ownership of its own buffers.
- Node handles passed to a validator point into the candidate tree and are
  only valid for the duration of the `commit()` call — validators must not
  retain them.
- After a successful commit the candidate is discarded (fresh empty tree),
  so staging is a *transaction*, not a scratchpad.

## Commit / rollback semantics

`cog_cfg_commit()`:

1. If nothing is staged (candidate clean) → successful no-op; running and
   the rollback slot are untouched.
2. Run every registered validator against the candidate. A validator on
   path `P` fires only when `P` is **dirty** in the candidate (changed by
   `set`/`delete` since the last commit/rollback) — untouched subtrees are
   not re-validated. A subtree deleted outright no longer exists and is
   skipped. An empty registration path (`""`) is a whole-tree validator
   that fires whenever anything is staged. Validators run in registration
   order; the first rejection aborts.
3. If any validator rejects → the staged changes are **discarded** and
   `COG_CFG_EVALIDATE` is returned. The session candidate is re-derived
   from the running tree, which is still the previous known-good. This is
   "fail closed": a bad proposed configuration is never half-applied.
4. Otherwise the validated candidate is **promoted** to running (no extra
   copy) and the old running tree is demoted to the rollback slot
   (`saved`). A fresh clean candidate is deep-copied from the new running
   tree *before* any live pointer moves, so an allocation failure mid-
   commit still leaves everything untouched and retryable.

`cog_cfg_rollback()` restores `saved` as the running tree, discards
uncommitted candidate changes, and re-derives the candidate from the
restored tree (a further commit after rollback is therefore always
possible). The slot is single-slot: a second consecutive rollback returns
`COG_CFG_ENOROLLBACK`, and any subsequent commit re-arms it.

### Why copy-on-commit?

The PoC uses **copy-on-commit** (deep-copy the candidate, swap pointers)
because it makes atomicity trivial to reason about: nothing in the running
tree is ever mutated in place, so there is no window in which a reader can
observe a partial configuration. The cost is an `O(n)` copy per commit and
duplicate memory between the rollback slot and the running tree.

For a production `libraries/cog-config` we would revisit this:

- **Copy-on-write nodes** (shared immutable subtrees with refcounts) reduce
  commit cost to only the changed paths — a natural fit given the
  tree-shaped data and the "read-mostly, write-rarely" profile.
- **A revision stack** instead of a single rollback slot, matching VyOS's
  ability to roll back across several commits.
- **Persistence/freezing**: serialise the running tree to disk so a crash
  never loses the last known-good config.

None of these change the public API shape validated here.

### Deliberate simplifications

- Single-slot rollback: a second consecutive `rollback()` returns
  `COG_CFG_ENOROLLBACK`; a new commit re-arms the slot. (Production would
  keep a revision stack, matching VyOS's multi-step rollback.)
- `delete()` prunes empty, valueless ancestors (VyOS-like), so deleting
  the last leaf under `providers.openai` removes the whole subtree.
- Setting an intermediate (container) node's value is allowed; `show()`
  prints every node that carries a value as `path = value`.
- Duplicate registration of the same path is allowed; validators run in
  registration order.

## Error codes

| Code                     | Meaning                                          |
|--------------------------|--------------------------------------------------|
| `COG_CFG_OK`             | Success.                                         |
| `COG_CFG_EINVAL`         | NULL argument, malformed path, zero-size buffer. |
| `COG_CFG_ENOMEM`         | Allocation failure.                              |
| `COG_CFG_ENOTFOUND`      | Path absent, or container without a value.       |
| `COG_CFG_EVALIDATE`      | A validator rejected the candidate.              |
| `COG_CFG_ENOROLLBACK`    | No previous running tree to restore.             |
| `COG_CFG_EBUFSMALL`      | Value does not fit the caller's buffer.          |

Every public function returns one of these; `cog_cfg_strerror()` renders a
human-readable message.

## Building and running

```bash
cmake -B build -DCOG_BUILD_POC_CONFIG_TREE=ON
cmake --build build
ctest --test-dir build -R config-tree -V        # unit/acceptance tests
./build/pocs/config-tree/config-tree-demo       # narrative walk-through
```

Both targets build with the project's strict warning set
(`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wmissing-prototypes
-Wstrict-prototypes ...`). Enable `-DCOG_ENABLE_WERROR=ON` and
`-DCOG_ENABLE_ASAN=ON` to prove warning- and memory-cleanliness.

## Files

| File            | Purpose                                              |
|-----------------|------------------------------------------------------|
| `config_tree.h` | Public API — the contract this PoC proves.           |
| `config_tree.c` | Node tree, path resolution, commit/rollback engine.  |
| `tests.c`       | Assertion driver; happy path, all error codes, edges.|
| `demo.c`        | Narrative acceptance walk-through.                   |
| `README.md`     | This document.                                       |

## Acceptance check

- [x] `set(path, value)`, `delete(path)`, `show()`, `commit()`, `rollback()`
      on a dotted-path tree.
- [x] Commit copies candidate to running after running registered validators.
- [x] Rollback restores the previous running tree.
- [x] C data structure and memory ownership model documented.
