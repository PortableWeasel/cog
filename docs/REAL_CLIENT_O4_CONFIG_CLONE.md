# Exact resolved configuration mirrors (3a1d5a5; duplicate 8840e0b cancelled)

Pass 1: coordinator/executor ownership requires two independent immutable owners
for one logical generation. The explicit private clone constructor copies resolved
state without running startup precedence, credential resolution or provider binding.
The executor consumes one owner; runtime owns the other until matching mirror
retirement. Copies are transport bookkeeping, not a third logical generation.

The clone preserves ROOT startup prompt/queue, session selectors and MOCK inputs.
It also preserves reload, child and bootstrap effective state without widening
permissions. Secret and strings are independently owned; immutable fixture objects retain
their documented external borrows. Transport is an explicit replacement borrow. Provider aliases are rebuilt
only by normal bind on the new owner. No public API, wire or process role changes.

Pass 2 scenario traces: (1/4) finish startup attachment once, clone exact resolved
ROOT including task/queue, then consume original into executor; either owner may
be freed first after its own provider disconnect. (3/9/10) copying child state
preserves narrowed masks/parent semantics and never creates a child. (8/12)
prepare candidate once, clone same generation, transfer executor candidate and
retain mirror; refusal frees candidate copies without changing current owner.
(5/7/16) allocation failure clears partial secret/string storage and leaves source
unchanged, no provider work; cleanup remains explicit. (11/17) migration clone
preserves resolved bootstrap state and never reloads startup input from sources.
(2/6/13/14/15/18/19/20) no state-machine, persistence, provider, process, tool,
observability or terminal effects are introduced. Existing owners retain their borrow lifetimes. Each new copy retains only its
explicit net borrow and existing fixture borrows; NULL mirrors borrow no net.

Trace amendment: clone is never eligible for finish_host. An unfinished original
must first complete host attachment before cloning a ready runtime mirror; clone
accepts explicit existing host net or NULL without constructing transport.

Pass 3 consistency: same generation is intentional and does not allocate an ID;
only current/future logical generations remain live. clone/free is paired, output
is NULL on any failure, and source bytes/options/binding remain unchanged. Borrowed
fixture pointers remain exact; transport is the explicit clone argument; owned pointer values may differ but bytes and
nullable semantics are exact. Safe labels use existing secret screening. Clone
must not memcpy bound aliases or expose the source secret. No new runtime callback,
mutable global, dispatch table, public API, wire field or source resolver appears.
Construction supports REAL and MOCK as well as metadata without starting work.
Error ordering is required output/current pointers first, then bounded allocation
and copying; failures publish no partial owner. Body implementation and fault tests
are deferred until independent header approval.

## Transport lifetime correction before independent gate

Targeted pass 1 — contract: clone_new(out, current, net) copies exact resolved
values while explicitly attaching the supplied host transport borrow. NULL is
valid for an unbound coordinator mirror; it does not permit REAL binding. MOCK
and metadata require NULL. No existing provider or source owner is rebound.

Targeted pass 2 — retirement trace: coordinator creates a NULL-net mirror before
execution_new consumes source configuration. Source executor finishes and its
provider disconnects; source configuration/net are destroyed. The mirror remains
valid because it has neither source net nor source adapter/endpoint aliases. A
new local slot clones mirror with its own actual net, binds normally, and owns
that borrow until provider/config teardown. Remote transfer remains the existing
bootstrap path. Startup prompt/queue, same generation, masks and secrets survive
copy exactly; no hidden startup replay is introduced by this constructor.

Targeted pass 3 — failure and consistency: NULL-net REAL mirror binding refuses
before any provider work. NonNULL MOCK/metadata attachment is INVALID_ARG and
leaves out NULL. Any allocation/copy failure leaves source and supplied net
unchanged and clears partial owned secret storage. Freeing a clone never frees
net; observer/transport remain host-owned. Clone-from-clone permits attachment of
a new explicit net without resurrecting source pointers or resolver authority.
No third logical generation, new source resolution, public API or wire change.
These corrections supersede the initial exact-net-pointer wording at547983a.
