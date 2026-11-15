# cog-core module contracts

Per `docs/IMPLEMENTATION_STRATEGY.md`, every public header module gets a
`CONTRACT.md` that lists its public types, constants/functions, error
codes, threading model, ownership rules, and invariants, together with
the test that verifies each claim.

Layout: one directory per module, named after its public header.

- `result/CONTRACT.md` — verified by `tests/unit/test_result.c`
- `message/CONTRACT.md` — verified by `tests/unit/test_message.c`
- `session/CONTRACT.md` — verified by `tests/unit/test_session.c`
- `event/CONTRACT.md` — verified by `tests/unit/test_event.c`
- `state/CONTRACT.md` — verified by `tests/unit/test_state.c`
- `handler/CONTRACT.md` — verified by `tests/unit/test_handler.c`

The contract is written against the header only; it never expands the
public API.  Cross-module composition of the Phase 1 primitives is
verified separately by `tests/integration/test_core_primitives.c`.
