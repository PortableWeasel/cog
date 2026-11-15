# session.h Contract

`libraries/cog-core/include/cog/session.h` — hierarchical conversation tree
with forking, timing, and billing.

This document records the executable contract verified by
`tests/unit/test_session.c` and implemented by
`libraries/cog-core/src/session.c` (ticket 0f055f5, Stage 12.1a). It
intentionally does **not** expand the public API; it documents what the
header declares and pins the implementation's interpretation of every
ambiguous point so the Critic can review the choices.

## Status

Implemented. Guard: `COG_SESSION_H`. Includes `<cog/event.h>`,
`<cog/message.h>`, `<cog/result.h>`, `<stdbool.h>`, `<stddef.h>`,
`<stdint.h>`.

## Public types

| Type | Kind | Notes |
|------|------|-------|
| `cog_session_t` | opaque struct | Created by `cog_session_new`, destroyed by `cog_session_free`. |
| `cog_session_node_t` | opaque struct | Owned by the session; never freed directly. |
| `cog_session_config_t` | struct | `id` (copied; NULL → generated) and `max_nodes` (0 → default 1024). |
| `cog_timing_t` | struct | Six fields: five `uint64_t` ms values + `tokens_per_second` double. |
| `cog_cost_t` | struct | Token counts, per-token prices, `total_cost`. |
| `cog_node_role_t` | enum | `COG_NODE_SYSTEM=0`, `USER=1`, `ASSISTANT=2`, `TOOL_CALL=3`, `TOOL_RESULT=4`. |
| `cog_node_data_t` | struct | Read-only view of a node; owned by the session. |

## Constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `COG_NODE_SYSTEM` | `0` | System instruction. |
| `COG_NODE_USER` | `1` | User input. |
| `COG_NODE_ASSISTANT` | `2` | Model output. |
| `COG_NODE_TOOL_CALL` | `3` | Tool invocation (name + JSON args). |
| `COG_NODE_TOOL_RESULT` | `4` | Tool result content. |

## Functions and error codes

Every function returns `cog_result_t` except the documented `void`/`bool`/
`double`/pointer accessors. Codes marked **(ext)** are fail-closed
extensions not listed in the header's `@return` block but consistent with
`cog/result.h`; they are pinned by the tests.

| Function | Success | Errors |
|----------|---------|--------|
| `cog_session_new` | `COG_OK` | `NULL_PTR` (out NULL), `NOMEM` |
| `cog_session_free` | — (void) | NULL is a no-op |
| `cog_session_id` | id string | NULL when session NULL |
| `cog_session_reset` | `COG_OK` | `NULL_PTR`, `INVALID_STATE` (reserved, see below) |
| `cog_session_is_empty` | bool | true when session NULL |
| `cog_session_root` / `cog_session_active` | node | NULL when empty or session NULL |
| `cog_node_parent` / `cog_node_first_child` / `cog_node_next_sibling` | node | NULL when absent or node NULL |
| `cog_node_child_count` | size_t | 0 when node NULL |
| `cog_node_data` | `const cog_node_data_t *` | NULL when node NULL |
| `cog_session_add_message` | `COG_OK` | `NULL_PTR`, `NOMEM`, `INVALID_ARG` (ext: malformed role), `NOT_FOUND` (ext: foreign parent) |
| `cog_session_append` | `COG_OK` | `NULL_PTR`, `NOMEM`, `INVALID_ARG` (ext), `NOT_FOUND` (ext) |
| `cog_session_add_tool_call` | `COG_OK` | `NULL_PTR` (session/tool_name/tool_args NULL), `NOMEM`, `NOT_FOUND` (ext) |
| `cog_session_add_tool_result` | `COG_OK` | `NULL_PTR` (session/parent/tool_result NULL), `NOMEM`, `NOT_FOUND` (ext) |
| `cog_session_fork` | `COG_OK` | `NULL_PTR`, `NOMEM`, `NOT_FOUND` (ext: foreign node) |
| `cog_session_get_history` | `COG_OK` | `NULL_PTR` (any of 4 args), `NOT_FOUND` (ext: foreign node), `NOMEM` |
| `cog_session_set_timing` / `cog_session_set_cost` | `COG_OK` | `NULL_PTR`, `NOT_FOUND` (ext: foreign node) |
| `cog_session_cumulative_cost` | double | 0.0 when session/node NULL or node foreign |
| `cog_session_cumulative_timing` | `COG_OK` | `NULL_PTR`, `NOT_FOUND` (ext: foreign node) |
| `cog_session_parent_id` | string | NULL when session NULL or no parent |
| `cog_session_set_parent` | `COG_OK` | `NULL_PTR` (session NULL), `NOMEM` |
| `cog_session_discard_node` | `COG_OK` | `NULL_PTR`, `INVALID_STATE` (node has children), `NOT_FOUND` (foreign node) |

`COG_ERROR_NOMEM` is not injectable through this API (no allocation
failure hook), so it is documented but not exercised by the tests.

## Ownership

- The session owns every node and every string it stores. All content,
  tool, raw-data and metadata strings are deep-copied on insertion and
  released by `cog_session_free`, `cog_session_reset`, discard, and
  pruning. No hidden state exists outside the session struct.
- `cog_node_data` returns a pointer into the node; it is valid until the
  session is structurally modified (node freed).
- `cog_session_get_history` returns a pointer into a single per-session
  buffer; it is valid until the next structural modification **or** the
  next `cog_session_get_history` call (the buffer is rebuilt in place).
- `cog_session_set_parent` copies the id; `cog_session_new` copies
  `config->id`.
- On failure, output parameters (`*out`, `*out_node`, `*out_msgs`,
  `*out_count`, `*out`) are left untouched.

## Invariants

- Node ids are unique and stable for the lifetime of the session; they
  start at 1 and never repeat (discard does not reuse ids; reset restarts
  at 1).
- `parent_id` in node data is 0 for the root and otherwise equals the
  parent's `node_id`.
- The active node is the most recently added node (add/append/tool
  call/tool result/fork all advance it). Discarding the active node moves
  it to the discarded node's parent.
- The tree is always consistent: no orphaned nodes, parent/child/sibling
  links agree with `child_count` and `node_count`.
- Content is length-prefixed: `content_len` is authoritative and embedded
  NUL bytes are payload. A NULL content with zero length is preserved as
  NULL; a non-NULL content with zero length is preserved as an empty
  string.

## Design decisions (interpretations of the header)

These are the points where the header is ambiguous; the implementation
picks one reading and pins it with tests.

1. **A new session is empty.** `cog_session_is_empty` is true and
   `cog_session_root`/`cog_session_active` are NULL after `cog_session_new`.
   The first `cog_session_add_message(s, NULL, ...)` creates the root.
   (docs/WALKTHROUGH.md describes a root node created at `session_new`;
   the header's empty-session contract supersedes it.)

2. **Fork copies the fork-point node.** `cog_session_fork` deep-copies the
   fork-point node's data (role, content, tool fields, timing, cost) into a
   new node inserted as the next sibling of the source, with no children.
   The new branch therefore shares all ancestors by reference and owns its
   own children, matching the header's "shares all ancestor nodes but has
   its own children". Forking the root is a no-op that returns the root
   itself (the root is already the shared ancestor of every branch).

3. **History tool mapping.** `COG_NODE_TOOL_CALL` and
   `COG_NODE_TOOL_RESULT` both map to `COG_MESSAGE_ROLE_TOOL` in the
   history: the call contributes `tool_args` as content with `tool_name`
   as name; the result contributes `tool_result` as content. A result node
   created via `cog_session_add_tool_result` (which takes no name) has a
   NULL `tool_name`, so its history message name is NULL. A result created
   via `cog_session_add_message` with `COG_MESSAGE_ROLE_TOOL` carries the
   message's `name`.

4. **History cache.** The returned message array is a per-session buffer
   rebuilt on demand; the implementation updates it through the
   const-qualified session pointer (the const is a caller-side view).

5. **max_nodes is a soft cap.** Zero selects the default of 1024. After
   each insertion, the oldest leaf nodes (smallest `node_id`, excluding
   the root and the active node) are pruned until the count fits. If only
   the root/active remain, the cap is exceeded rather than corrupting the
   tree.

6. **Reset semantics.** `cog_session_reset` frees all nodes, empties the
   tree, restarts node ids at 1, and retains the session id **and** the
   parent session id (session identity persists; only conversation nodes
   are cleared). Its `COG_ERROR_INVALID_STATE` path is reserved for the
   core integration layer: the session keeps an internal `attached` flag
   that no public session API sets, so the error is not reachable through
   this module alone (core.c, ticket 681a566, will own the flag).

7. **Reserved node fields.** `raw_request`, `raw_response`, `model`,
   `provider`, and `chunk_count` are always NULL/0: the header declares no
   setter for them. They are preserved for persistence/debugging and will
   be populated by a future ticket.

8. **Foreign-node rejection.** Any node/parent passed to a session that is
   not reachable from that session's root is rejected with
   `COG_ERROR_NOT_FOUND` (fail closed rather than corrupting the tree or
   writing through a stale pointer).

9. **Discarding the root.** A leaf root may be discarded; the session
   becomes empty and a subsequent add creates a new root (with a fresh,
   non-reused id).

10. **Cumulative timing** sums every field of `cog_timing_t` literally
    along the path (including timestamps); only `duration_ms`/`ttft_ms`/
    `tokens_per_second` are meaningful as totals. `cumulative_cost` sums
    `total_cost` per node.

11. **No persistence in this module.** session.h declares no
    save/load/serialize functions; scenario 13's `cog_session_save` /
    `cog_session_load` / `cog_session_deserialize` belong to
    `cog/session_store.h` (Stage 12.2d, ticket 0efef06). No round-trip
    tests apply here.

## Threading

Per header: a session is owned by exactly one core instance and is not
thread-safe. All access happens on the thread that runs `cog_core_run`.

## Verification

Commands (run from the repo root; shared CMake is owned by ticket
6044b51, so the module is compiled directly with the project's strict
flags):

```bash
# Implementation, strict flags
gcc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    -Wsign-conversion -Wmissing-prototypes -Wstrict-prototypes \
    -Wundef -Wpointer-arith -Wcast-align -Wwrite-strings \
    -Wlogical-op -Wformat=2 -Werror \
    -c libraries/cog-core/src/session.c -o /tmp/session.o \
    -I libraries/cog-core/include -I libraries/cog-ipc/include

# Test, strict flags, then run
gcc -std=c11 <same flags> -c tests/unit/test_session.c -o /tmp/test_session.o \
    -I libraries/cog-core/include -I libraries/cog-ipc/include
gcc /tmp/test_session.o /tmp/session.o -lm -o /tmp/test_session
/tmp/test_session          # -> "test_session: ok", exit 0

# ASan + UBSan + leak detection
gcc <same flags> -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -c libraries/cog-core/src/session.c -o /tmp/session_asan.o \
    -I libraries/cog-core/include -I libraries/cog-ipc/include
gcc <same flags> -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -c tests/unit/test_session.c -o /tmp/test_session_asan.o \
    -I libraries/cog-core/include -I libraries/cog-ipc/include
gcc -fsanitize=address,undefined -fno-omit-frame-pointer \
    /tmp/test_session_asan.o /tmp/session_asan.o -lm -o /tmp/test_session_asan
ASAN_OPTIONS=detect_leaks=1 /tmp/test_session_asan   # -> ok, no leaks
```

`tests/unit/test_session.c` covers: lifecycle and NULL contracts; add
message (root/child/sibling, embedded NULs, NULL content, TOOL role,
invalid role, foreign parent); append; tool call/result (happy path and
NULLs); multi-level navigation; node data fields and reserved fields;
fork (leaf, mid-tree, root, NULLs, foreign, branch divergence, timing/cost
preservation); history (single, chain, tool mapping, NULLs, foreign,
rebuild-overwrites); timing/cost set and cumulative sums; parent id
get/set/clear; discard (leaf, active, non-leaf, foreign, NULLs, root,
history-after-discard); max_nodes pruning and default; stable node ids.
