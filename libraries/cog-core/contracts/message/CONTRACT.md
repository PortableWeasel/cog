# message.h Contract

`libraries/cog-core/include/cog/message.h` — chat message types used in
provider requests.

This document records the executable contract verified by
`tests/unit/test_message.c`. It intentionally does **not** expand the
public API; it only documents what the header already declares.

## Status

Header-only. No functions, no state, no implementation units required.
Guard: `COG_MESSAGE_H`. Includes `<stddef.h>` for `size_t`.

## Public types

| Type | Kind | Notes |
|------|------|-------|
| `cog_message_role_t` | `enum cog_message_role` | Conversation role of a message. |
| `cog_message_t` | `struct cog_message` | One chat message; a plain caller-owned value. |

## Constants (roles)

| Constant | Value | Meaning (from header) |
|----------|-------|------------------------|
| `COG_MESSAGE_ROLE_SYSTEM` | `0` | System instruction. |
| `COG_MESSAGE_ROLE_USER` | `1` | User input. |
| `COG_MESSAGE_ROLE_ASSISTANT` | `2` | Model output. |
| `COG_MESSAGE_ROLE_TOOL` | `3` | Tool result; use `name` to identify the tool call. |

## Struct layout

```c
typedef struct cog_message {
    cog_message_role_t role;      /* Role of this message. */
    const char *content;          /* Message payload; may be NULL when
                                     content_len is zero. */
    size_t content_len;           /* Length of content in bytes. */
    const char *name;             /* Tool name for COG_MESSAGE_ROLE_TOOL;
                                     may be NULL otherwise. */
} cog_message_t;
```

## Functions

None. The module defines the types and constants only.

## Ownership

- The `content` pointer is **referenced, not owned**. The caller must
  ensure the content outlives the request that contains the message
  (per header).
- `name` is likewise referenced; it points at a tool name string owned
  by the caller.
- The struct itself is a caller-allocated value: it needs no
  constructor, destructor, or free function.

## Invariants

- `content` is length-prefixed: `content_len` is the payload length in
  bytes, so content may contain embedded NUL bytes and need not be
  NUL-terminated.
- `content` may be NULL when `content_len` is zero (per header). In
  particular, a zero-initialized message (`(cog_message_t){0}`) is a
  valid empty message: role `COG_MESSAGE_ROLE_SYSTEM`, content NULL,
  content_len 0, name NULL.
- `name` must identify the tool call when role is
  `COG_MESSAGE_ROLE_TOOL`; for the other roles `name` may be NULL.
- Messages are read-only after construction: the struct stores
  `const char *` pointers, so a message cannot mutate the payload it
  references. Mutating the underlying strings after a message is shared
  with a provider is a caller error.

## Threading

Per header: message structs are read-only after construction and may be
shared across threads as long as the underlying content strings are not
modified. Individual provider implementations must not retain message
pointers beyond the call that receives them unless they copy the data.

## Error semantics

None. message.h declares no functions and returns no error codes.
Semantic validation (e.g. rejecting a NULL content with non-zero
length) belongs to consuming modules.

## Verification

`tests/unit/test_message.c` pins the four role values, the struct field
types (via type-generic selection) and member count, and checks at run
time: role ordering 0..3 and switch usability, zero-initialized-message
validity, length-prefixed content with embedded NULs, the
TOOL-role-uses-name convention, and read-only usage through `const
cog_message_t *`.
