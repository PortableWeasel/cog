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

## Pass 2: targeted entry-point traces

| State / call | Required trace and result |
|---|---|
| No directory, NEW CAS | Existing locked directory bootstrap; six ENOENT facts; publish epoch1/checkpoint0; OK. |
| Owned checkpoint0, restart CAS | Exact old tuple/token and lock checked first; six ENOENT facts; publish next epoch; OK. |
| Either owner state, any lone recognized name | Probe sees object; CAS returns UNSUPPORTED before publication; owner/token unchanged. |
| Old owned checkpoint0, acquire | Same owner under lock, then recognized artifact refusal; no guard escapes on failure. |
| Missing metadata, other recognized artifact, load | Probe sees ambiguity; UNSUPPORTED with no session/report publication, never an empty reconstructed session. |
| Missing metadata, other recognized artifact, save | Existing guard/owner rules first; probe refuses before generation installation; no legacy bytes or manifest changed. |
| Present metadata, load/save | Probe then existing strict v1 metadata/nodes parsing and monotonic migration; malformed, missing or unidentified effect data retain existing errors. |
| Valid v1, bootstrap CAS | Refused until explicitly saved as unowned v2 and supplied with its exact token. |
| Valid v2 checkpoint plus leftover legacy files | Existing generation checksum/snapshot validation remains authority; no legacy probe changes load, CAS, acquire or save. |
| Unreferenced v2 generation/tmp files only | Six recognized legacy names absent; old v2 orphan-generation collision avoidance still applies, no snapshot inference. |
| Concurrent live guard | Same lock returns AGAIN before inspection/publication; no replacement/adoption. |
| Inspection failure after one or more absences/presences | Any nonENOENT stat error returns IO; output unchanged; no owner or data publication. |

The matrix includes all six names separately, regular files, directories and
dangling symlinks, both absent-owner and existing-owner checkpoint0 paths, direct
guard acquisition, and unowned/guarded first save. Ordinary complete legacy
migration, incomplete metadata, active-guard contention and authoritative-v2
leftovers provide positive/control coverage. Test injection targets directory-
relative stat only, preserving real backend locks, manifests and transaction I/O.
