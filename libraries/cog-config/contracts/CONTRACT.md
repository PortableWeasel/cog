# Bounded configuration contract

The public contract is [config.h](../include/cog/config.h). Delivery J implements
its constructor-reserved aggregate profile without changing that header.

`cog_config_tree_new_ex` reserves nodes, transaction identities, ordered validator
slots, a stable string arena, and all operation scratch storage. Zero limits select
8192 nodes, 8 MiB string bytes, 4096 transaction identities and 16 validators.
The node/string ceilings cover the running tree and every active candidate together;
each root consumes one node. Validator prefix strings share the same string ceiling.
Finalized handles consume identity slots until tree destruction, while their candidate
nodes and strings are reusable. No handle identity is recycled.

Every operation, including reads and begin, runs on the owner's thread. Runtime
operations perform no allocation, file I/O, implicit initialization or pool growth.
Node traversal and deletion are iterative, including deep paths. Caller-provided
output overflow is distinct from constructor workspace exhaustion.

The string arena uses first-fit contiguous slices with constructor-reserved occupancy
and admission maps. It does not relocate strings borrowed by other active snapshots.
A string needs one contiguous slice; fragmentation may therefore return LIMIT even
when the sum of free bytes is larger. A trial admission map and staged path/value
copy make set atomic, including aliased input from the same candidate. The implementation
reserves three bytes of string scratch/bookkeeping per configured string byte in
addition to the live arena; these bytes are fixed constructor overhead, never extra
capacity for live configuration. Node remapping scratch is likewise constructor-owned.
All size products and the aggregate reservation sum are checked before allocation.

Begin clones the complete running tree into reserved slots. Capacity failure returns
LIMIT, leaves `*out_txn=NULL`, preserves running/other candidate data and consumes no
transaction identity. Successful commit validates in registration order, then promotes
the candidate without a fallible step. A validator is called only if its prefix exists;
empty prefix means the root and always runs. Rejection discards staged edits and resets
the candidate to running. If that reset cannot fit, commit returns LIMIT and finalizes
only that transaction; running and other snapshots remain intact.

Set creates intermediate nodes and copies values. Delete removes a subtree and prunes
valueless empty ancestors. Root listing is count-only when capacity is zero; a short
nonzero key array returns the complete required count and writes no keys. Child order
is insertion order. Paths use the declared nonempty dotted-segment grammar.

Successful commit and rollback finalize a handle. Subsequent mutations/commit report
INVALID_STATE; get/list report NOT_FOUND; rollback is harmless. Borrowed values and
keys remain valid until mutation/finalization of their candidate or tree destruction.
Other snapshots' mutations, commits, begins and validator registrations do not move them.

Validator slots return OVERFLOW when full, and LIMIT when their prefix cannot fit.
Registration failure consumes no slot/string capacity. Constructors return NOMEM and
clear the output pointer on allocation failure; all partial reservations are released.
Tree free releases active candidates and retained tombstones together.

Executable evidence:

- `tests/unit/test_config.c`: baseline public behavior, ordering, validators, 80-segment
  paths, 32 concurrent snapshots and repeated commits/rollbacks.
- `libraries/cog-config/tests/test_config_bounded.c`: exact node/string/identity limits,
  atomic rejected set/begin, aliased replacement, prefix limits, snapshot pointer
  stability, reusable rejection, and rejected-reset LIMIT finalization.
- `applications/cog-cli/tests/test_configuration_memory.c`: linker-local allocation
  failure at each of the nine constructor allocations, leak accounting, and an armed
  allocation failure across begin/set/get/list/delete/commit/rollback/registration that
  proves none of those operations calls malloc/calloc.

The memory-injection target is Linux GNU/Clang specific; the production API exposes
no allocator hooks or process-global allocator state.
