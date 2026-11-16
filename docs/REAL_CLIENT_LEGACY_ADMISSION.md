# Checkpoint-zero legacy absence proof

Ticket `9a95735`, supporting empty-session recovery `cd3d092`, base `f3356c1`.
Existing public ownership admission requires no owner/data for bootstrap and
explicit migration before legacy v1 can acquire execution authority. Checking
only meta.json cannot prove absence: a lone nodes.jsonl may describe unidentified
effects. This correction adds no public API and no application filesystem logic.

## Pass 1: private contract

`session_store_file.h` declares one bounded directory-relative probe. Its six
recognized names match the existing legacy deletion classifier: meta.json,
nodes.jsonl, index.bin, and each corresponding .tmp name. Presence of any object
at a recognized name counts, including symlinks and nonregular files. Only ENOENT
proves a name absent; any other inspection failure returns IO with output unchanged.

Index and legacy temporary files do not independently establish a replayable
snapshot, but they also cannot prove that a legacy session never existed. Without
authoritative metadata they therefore fail closed as UNSUPPORTED. The operator
must retain these artifacts for explicit investigation; this change never deletes
or fabricates missing data. Uncommitted v2 generation files, .generation.tmp and
.manifest.tmp remain nonauthoritative under the existing atomic-manifest profile.

At generation zero, CAS and execution-guard acquisition reject any recognized
legacy presence as UNSUPPORTED. A load or first save with missing meta.json and
other recognized artifacts also returns UNSUPPORTED, instead of NOT_FOUND or
silently publishing over it. If metadata exists, the existing strict v1 parser,
monotonic checks, and explicit unowned migration remain unchanged. A valid nonzero
v2 checkpoint stays authoritative and ignores leftover legacy files as before.

No new allocation, state, lock domain, mutable global, public error, automatic
adoption, migration or effect recovery is introduced. Admission/publication runs
under the existing independent-description session lock; load remains read-only.
