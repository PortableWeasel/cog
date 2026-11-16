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

## Pass 2: callback traces

- new → start(100) → poll(100): both callback samples are valid; equal clock
  values do not imply repeated start authority or a repeated child admission.
- start(100) → poll(99): INVALID_ARG; poll outputs and all handle/group/mailbox
  state remain unchanged. poll(101) then advances the same existing attempt.
- start(100) → cancel(120) → poll(119): the final poll is rejected without
  undoing cancellation or altering the retained result/receipt. poll(120) may
  continue actual cleanup. A second cancel(120) remains idempotent.
- new → cancel(0): permitted prepared cancellation uses the valid initial zero
  sample. A later start still fails its existing cancelled/state precondition;
  the clock field cannot turn a denied call into a started one.
- poll(130) returns a driver failure: retain130 for cleanup; cancel(129) is
  rejected, cancel(130) may request cleanup while preserving the first failure.
- two handles A/B: A observes200, B observes150 then160. B does not compare with
  A's sample or a later app clock. B rejects its own159 after160.
- timeout, final output copy, durable-result notification and destroy never
  repurpose the sample as a new deadline or permit a new effect.
