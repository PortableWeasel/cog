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

## Pass 3: consistency and edge review

The probe examines exactly six names and completes all inspections before
publishing its by-value result. ENOENT is evaluated immediately after each failed
stat, so stale errno from another call cannot establish absence. It follows no
symlink and reads no artifact content; any found object blocks zero-generation
ownership, while a found metadata object still receives the existing strict
read/type/parser validation on the legacy load/migration path.

The six-name table will also serve the existing deletion classifier, avoiding
divergent definitions. Deletion behavior itself is unchanged. Metadata-less
legacy artifacts are conservatively ambiguous remnants, not invented committed
authority. Valid v2 authority wins before the probe is considered. Unsupported
data and inspection failure never trigger artifact cleanup, rollback, adoption
or a reset to an empty snapshot. Guard construction and output session/owner
publication retain their existing all-or-failure behavior.

Existing locks serialize cooperating writers; these contracts do not claim to
make read-only legacy loads atomic against an external process modifying files
outside the store protocol. No broader filesystem scan or new lock is added.
Independent final header review is required before implementing the helper or
modifying its four production call paths.

## Implementation and writer verification

The independent header gate passed `4b9a3b9`, after passes `8518917`, `48e2d43`
and `4b9a3b9`. Implementation checkpoint `6f5aacc` adds the finite probe and
reuses its immutable names in the existing deletion classifier. There is no
public header change. Store behavior now matches the four traced entry points;
the application never inspects backend filenames.

`unit-store-legacy-admission` exercises six names × three object types (regular,
directory, dangling symlink) × two ownership states, preserving original object
identity/size and exact regular-file bytes after refusal. Guarded first save is
tested while the real live guard excludes CAS with AGAIN; after release, direct
acquire and CAS both refuse the remnant. Metadata-only load/save retains the
existing IO result for missing nodes; other lone names return UNSUPPORTED.

At each of the six names, injected EIO, EACCES and ELOOP are checked through
absent-owner CAS/unowned save and existing-owner CAS/acquire/load/guarded save.
Fault counts prove the hooks ran; owner/token and load report remain unchanged,
no session or guard escapes. The test uses real backend locks and transactions,
wrapping only directory-relative stat. Separate cases prove missing-directory
and empty-reservation NOT_FOUND, orphan-v2 generation collision avoidance,
same-process independent-store guard exclusion, valid explicit v1 migration,
and nonzero-v2 authority even when legacy inspection would fail. Existing strict
legacy parsing, persistence transaction/fencing and application suites also run.

Fresh all-target strict Release builds passed with GCC and Clang and a separate
Clang ASan/UBSan build. Each passed **52/52** tests selected by:

```sh
ctest --test-dir BUILD -R 'unit-store|session|cli-' --output-on-failure -j4
```

Builds: `/tmp/cog-legacy-gcc`, `/tmp/cog-legacy-clang`, `/tmp/cog-legacy-asan`.
All pin CURL_LIBRARY and COG_NET_CURL_LIBRARY to the accepted installed
`/tmp/cog-relocatable-profile/shipped/curl-build/install/lib/libcurl.so`,
COG_NET_CURL_BUILD_DIR to its audited `curl-build`, and CURL_INCLUDE_DIR to that
installation's `include`. They use COG_ENABLE_WERROR, with COG_ENABLE_ASAN only
in the separate sanitizer build, ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 and
UBSAN_OPTIONS=halt_on_error=1. Test assertions remain enabled in Release.

Independent final source review remains required. This scoped correction does
not itself implement empty application checkpoint recovery or close M/N/O/P.
