# cog-core module contracts

Per `docs/IMPLEMENTATION_STRATEGY.md`, every public header module gets a
`CONTRACT.md` that lists its public types, constants/functions, error
codes, threading model, ownership rules, and invariants, together with
the test that verifies each claim.

Layout: one directory per module, named after its public header.

- `result/CONTRACT.md` — verified by `tests/unit/test_result.c`
- `message/CONTRACT.md` — verified by `tests/unit/test_message.c`

Later phases add `session/`, `event/`, `state/`, `handler/`, etc. The
contract is written against the header only; it never expands the
public API.
