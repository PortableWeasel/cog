# Stage 14 replay primitive implementation evidence

Ticket 91108c5 implements the bounded in-memory replay owner and CGR1 codec
specified by `cog/ipc.h` and scenario 11/R13. The owner reserves metadata and
payload storage at construction. Successful append copies bytes, evicts only
the oldest records needed, and never allocates; reads and codecs do not advance
shared state. Generation mismatch and lost prefixes yield explicit GAP records.

CGR1 payload length is a 32-bit wire field. Append accepts payloads up to the
configured in-memory bound even if a payload cannot fit that wire field; encode
returns LIMIT before reading such payloads or writing output. Consumers sending
CGR1 must configure a wire-compatible payload bound. This is an explicit codec
representation boundary, not a truncation or eviction rule.

`test_ipc_replay` covers retention and byte/entry eviction, empty and stale
windows, cursor bounds and exhausted sequences, binary and zero-length payloads,
copy ownership and capacity retry, wire validation, UTF-8 and version/kind
rejection, and each constructor allocation failure. Linker allocation wrappers
verify append, copy, encode and decode perform no runtime allocation. Strict
GCC, Clang and ASan/UBSan focused runs provide local evidence. Full application
reconnect/snapshot behavior remains the separate O integration task.
