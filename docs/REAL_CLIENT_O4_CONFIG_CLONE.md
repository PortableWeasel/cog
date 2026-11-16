# Exact resolved configuration mirrors (8840e0b)

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
