# Generic reply delivery retention

Ticket81431a3 supplements approved92a7d46. Header/docs only; source use gated.

## Pass 1: independent delivery owner

A generic incoming lane may retain a semantic request pending resolution while
also owning a provisional/final response. copied_wire/next_send_ms describe when
one retained wire may be copied again, but do not say whether a duplicate has
re-armed a response. pending_valid must keep its semantic request meaning where
used that way. PROFILE already has its independent dedicated pending flag and
needs no added field.

Add only generic lane pending_reply_wire:0 means no response waiting for delivery;
a nonzero value selects the exact existing final_reply_wire or provisional_wire
and immutable buffer. Arm it after constructing a valid response, or after a
validated duplicate requests its retained response. A deferred send or AGAIN
preserves selector, wire, bytes and existing send eligibility. Successful copied
send clears only the matching selector; it neither consumes semantic request nor
retires receipt proof. Later validated duplicate may re-arm the same wire while
preserving its copied_wire/next_send_ms cooldown.

A fair producer pass services an armed reply when eligible, even if no further
request arrives. When ineligible, its next_send_ms is included in the owner step's
next_due bound. Different streams keep independent selectors. This introduces no
buffer, ACK, new frame identity, wire kind, hot allocation or extra native read.
