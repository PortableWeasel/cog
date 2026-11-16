# Delegation handle callback clock

Private layout clarification49e2da0 adds only `last_now_ms` to
`cli_delegation_handle`. No API, opcode, callback or execution authority changes.
The existing public async tool contract in `cog/tool.h` requires poll and cancel
to reject backwards caller timestamps with INVALID_ARG. Start consumes its one
start operation only after normal admission checks.

The current `deadline_ms` is the finite original parent/tool deadline and cannot
also store the previous callback time. App/context time is not a substitute:
an explicit direct callback may have observed a newer timestamp than app time,
and two handles must not reject one another's independent monotonic samples.
The handle owns its own scalar for exactly its constructor-to-destroy lifetime.

## Pass 1: storage and validation

Constructor initializes last_now_ms to0; timestamp0 remains a valid initial
sample. Required-pointer and callback-state checks run before clock validation.
A valid start/poll/cancel entry rejects now<last_now_ms before modifying handle,
caller outputs, retained receipts, reservations or work. Equal timestamps are
valid. After validation it records now before driving; a later driver failure
does not roll the clock back. The original deadline never extends as a result.
No clock sampling, new allocation or other shared mutable state is introduced.
