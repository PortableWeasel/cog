# H narrow mock fixture contract amendment

Ticket H `055211c`, implementation child `51be839`. Header-first addition:
`applications/cog-cli/include/cog_cli/mock_provider.h` declares
`cog_mock_provider_new_deltas` before its implementation. No real provider,
net, session, store or application public API is changed.

Pass 1 defines an explicit fixture constructor taking a borrowed immutable array
of typed provider deltas, a count and an owned provider output. Nested strings
remain borrowed until disconnect; an ending FINISH is mandatory. The constructor
reserves a fixed 128 KiB request arena and matches provider disconnect. Every
send copies the entire nested request into that arena, before returning.

Pass 2 retraces scenario15 and R04: separate TOOL_BEGIN/ARGUMENTS/END, optional
OPAQUE and FINISH events can model complete identified tool turns and a following
scripted answer. A FINISH separates successive sends. Script consumption happens
only on successful delivery. Short caller storage reports exact required bytes,
writes neither delta nor storage, and retains the same sequence across poll.
Cancellation after an overflow preserves that retained item then emits exactly
one ABORTED FINISH. An ordinary error also ends its turn; a later send proceeds
to the next scripted step. No script event executes a workspace tool.

Pass 3 checks consistency with mandatory generic eight-callback dispatch,
constructor/destructor pairing, owner-thread callbacks, nonblocking mock poll
and no mixing next/receive within a turn. `chunk_delay_ms` is a monotonic poll
schedule, never a sleep. Scripted text receive can combine its last text and
STOP completion for legacy consumers. Once terminal delivery is consumed,
receive/next returns INVALID_STATE until the next send. Script selection uses
endpoint.implementation_config, never the credential field.

Final targeted checks are `unit-mock-provider`: typed tool/opaque order,
transactional overflow, cancellation after overflow, legacy script errors,
repeat turns and endpoint selection. This is a fixture contract amendment,
not evidence that the pending K durable tool loop has passed R04/scenario15.

## H depth admission gap (coordinator-authorized narrow amendment)

Pass1 records a concrete implementation constraint in `cog/openrouter.h` before
review: the OpenRouter backend accepts max_json_depth1..256 and returns
INVALID_ARG for257 or greater. This is backend admission, not silent clamping.
Pass2 retraces R03/R15 with a depth256 opaque value and repeated equal metadata,
exercising strict parse, duplicate recursion and opaque equality; depth255 rejects
the same input before traversal. Pass3 checks ceiling257 constructor rejection,
no request effects, and unchanged generic nonzero-limit semantics for other
provider implementations. Sanitizer execution at256 is required before review.
