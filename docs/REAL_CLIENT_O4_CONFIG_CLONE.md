# Exact resolved configuration mirrors (3a1d5a5; duplicate 8840e0b cancelled)

Pass 1: coordinator/executor ownership requires two independent immutable owners
for one logical generation. The explicit private clone constructor copies resolved
state without running startup precedence, credential resolution or provider binding.
The executor consumes one owner; runtime owns the other until matching mirror
retirement. Copies are transport bookkeeping, not a third logical generation.

The clone preserves ROOT startup prompt/queue, session selectors and MOCK inputs.
It also preserves reload, child and bootstrap effective state without widening
permissions. Secret and strings are independently owned; net and immutable fixture
objects retain their documented external borrows. Provider aliases are rebuilt
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
observability or terminal effects are introduced. Existing owners retain their
borrow lifetimes; the runtime must keep net/fixture objects through BOTH copies.

Trace amendment: clone is never eligible for finish_host. An unfinished original
must first complete host attachment before cloning a ready runtime mirror; clone
retains exact existing net including NULL and cannot install new transport.
