# session_store.h Contract

The public contract is `libraries/cog-core/include/cog/session_store.h`.
Stage 14.G.2 implements the approved version 2 transaction and same-host fencing
profile. Independent review accepted `619fa0a` after the focused revisions;
merge `3c4e39c` integrates that branch with the existing F/Q deliveries.

## Constructor and dispatch ownership

`cog_store_new` validates every required save/load/append/delete/list/exists and
load_ex callback before construction. Fenced callbacks are a complete optional
group; an incomplete group or require_owner with an incapable backend returns
UNSUPPORTED. Compression is unsupported. A custom backend receives exactly the
borrowed `backend_context`, including NULL for stateless implementations. The
wrapper neither interprets nor destroys it. Builtin contexts are constructor-owned.

The file constructor copies base_dir (default `.`) and reserves four separate
max_record_bytes buffers (default 16 MiB each): encode, read, decode descriptors,
and exact immutable-node comparison. The reservation is an explicit per-buffer
ceiling, not an aggregate memory claim. Buffers never grow during operations.
Save, append, owner_read, CAS, guard borrows/returns and checkpoint save perform
no runtime allocation. Directory scans use Linux getdents64 into fixed caller
storage rather than opaque allocating DIR handles. List is an explicit caller-owned output constructor;
load/checkpoint_load call the explicit session restore constructor; owner_acquire
and owner_acquire_recovery construct a separately matched guard. Free requires
all guards released, and never silently releases live ownership.

The null backend is nondurable: save and append return UNSUPPORTED, load and
delete return NOT_FOUND, exists is false, and list constructs an empty list.
It cannot satisfy require_owner or authorize a durable tool intent.

## File representation and commit point

```
base_dir/.cog-domain.lock             stable ownership-domain anchor
base_dir/session_id/.owner.lock      stable independently opened session anchor
base_dir/session_id/manifest.bin     single atomic owner/data authority
base_dir/session_id/generation-00000000000000000001.bin
base_dir/session_id/generation-00000000000000000002.bin
```

All integer fields below are explicit little-endian uint64 values. Checksums
are FNV-1a 64-bit for corruption detection, not cryptographic authentication.
The authoritative manifest is exactly 64 bytes:

| Offset | Field |
| --- | --- |
| 0 | Eight-byte `COGMAN2\0` marker |
| 8 | Format version, 2 |
| 16 | Exact current immutable generation, or zero before first checkpoint |
| 24 | Complete generation file byte length, including envelope |
| 32 | Checksum of complete generation file |
| 40 | Owner epoch, or zero for unowned |
| 48 | Owner process incarnation, or zero for unowned |
| 56 | Checksum of preceding 56 manifest bytes |

Each immutable generation has a 40-byte envelope followed by the private codec
payload:

| Offset | Field |
| --- | --- |
| 0 | Eight-byte `COGGEN2\0` marker |
| 8 | Exact generation token |
| 16 | Payload byte length |
| 24 | Payload checksum |
| 32 | Checksum of preceding 32 envelope bytes |
| 40 | Complete `COGSNAP2` snapshot payload |

Publication first detaches any stale temporary name and exclusively creates a
fresh inode (never truncating an existing potentially hardlinked checkpoint).
Unlink errors fail closed. It writes `.generation.tmp`, fsyncs the complete file, installs an
immutable generation with linkat (never replacing an existing token), writes
and fsyncs `.manifest.tmp`, renames it over manifest.bin, and fsyncs the session
directory. The new manifest is the sole transaction commit point. Immutable
files left before publication are harmless orphans; generation selection probes
existing names and never reuses their tokens. Files retain mode 0600. New session/anchor
creation is fsynced through the session and base directories before admission.
A configured root created by the backend also has its actual parent directory
fsynced before ownership-domain creation/publication. Existing-root retries
confirm that parent again, so an earlier uncertain mkdir/fsync never turns into
an assumed durable root. Parent open/fsync failure returns IO before publication.
Readers validate exact size, checksums and decoded semantic associations; they
never substitute an older generation after corrupt authoritative data.

Prepublication errors return IO and leave the old manifest authoritative. A
failed final directory sync returns IO for save and INDETERMINATE for owner CAS;
the caller must not infer rollback or start effects. The held guard blocks new
execution borrows after uncertain save publication. An identical owner retry
revalidates and fsync-confirms the committed generation/manifest/directory before
returning OK. A later successful save can durably confirm a new transaction.

## Monotonic committed history

Every save/append, including direct builtin callbacks and require_owner=false,
locks and validates the current committed generation before comparing data.
The complete canonical codec bytes of each retained node must match: identity,
parentage, stable sibling order, legacy fields, structured messages, exact
opaque bytes, usage, timing, cost and provider diagnostic fields. All old nodes
remain, allocator/audit high-water never decreases, and newly introduced node
and audit identities exceed the committed high-water. A replacement object
using the same session ID cannot erase history or rebind identities.

Every prior ledger key and immutable audit prefix remains. Only approved
PROPOSED->STARTED/DENIED and STARTED->SUCCEEDED/FAILED/CANCELLED/INDETERMINATE
transitions are accepted by ordinary save. Final outcomes/results cannot change.
Existing ledger updates require save; append accepts exactly one new node with
unchanged prior ledger state and uses the same complete transaction. Unresolved
old work prevents unrelated newly inserted work. Invalid associations return
PROTOCOL; valid but nonmonotonic changes return INVALID_STATE, without publication.

Recovery-only saves allow only conservative STARTED->INDETERMINATE normalization
and explicit audited reconciliation of INDETERMINATE to a supported final state.
All old messages remain byte-exact; newly added nodes must be matching results
for preexisting uncertain calls, never new user/assistant/tool-call work. Recovery
guards refuse execution borrows, append and checkpoint save, even after the final
uncertainty is closed. Release, CAS a new epoch against the resulting exact
checkpoint, then acquire an ordinary guard to resume execution.

## Locks and ownership

Bootstrap and deletion serialize through the stable base ownership-domain lock.
All writers, CAS and acquisition use independently opened flock descriptions
with CLOEXEC; contexts in the same process contend. An ordinary live matching
guard holds the lock across save operations. Unguarded saves cannot change
owned state, regardless of require_owner. CAS returns AGAIN while a guard or
writer holds the anchor; it compares exact old owner and latest checkpoint,
checks monotonic epoch without wrap, and publishes owner+checkpoint in one
manifest transaction. Existing unowned v2 adoption validates exact safe data;
v1 must first be explicitly migrated to v2. Coherent PROPOSED may acquire a normal
guard. STARTED/INDETERMINATE requires the explicit recovery path.

No store path unlinks or replaces either anchor. Any session containing an
ownership anchor or manifest is undeletable through delete/delete_, including
unowned v2 or after every guard releases. Only unowned legacy deletion is
supported, with a complete known-file preflight before any removal. An operator
must not externally remove/replace lock anchors while the store is in use.
Callers fence/reap any old owned process group before admitting a fresh process
incarnation; the store does not pretend an OS lock alone proves remote effects.
Fork/exec callers must close inherited descriptors in their child before doing
work; CLOEXEC prevents retention after exec. Library code does not fork commands.

A guard retains its store/context lifetime and counts bounded execution
constructor borrows. Release refuses live borrowers. Checkpoint save also
requires no borrowers, no unresolved effects and a complete selected replay path.
Exact checkpoint load never substitutes latest and never activates execution.

## Load, legacy migration, paths and limits

Public loads decode into reserved workspace then call cog_session_restore to
validate full tree/ledger/audit/branch semantics and normalize committed STARTED
to INDETERMINATE. Raw codec decode deliberately retains STARTED for writer diffs.
Failed public load sets out=NULL; report and owner/checkpoint outputs remain
unchanged on failure. Load never rewrites disk. No advisory index is required.

The original v1 meta.json/nodes.jsonl input is preserved. Replay-safe text
migrates only in memory; explicit save publishes v2. Unidentified legacy tools
require INSPECT and cannot invent call IDs or authorize replay. Embedded-NUL
legacy fields that cannot be represented losslessly return UNSUPPORTED. Tail
recovery is optional and discards only one unterminated record beyond the
validated committed v1 count. v2 is a complete binary transaction: truncation,
trailing bytes and corruption fail closed even with tail recovery enabled.

Session IDs are nonempty printable path components excluding slash, backslash,
`.`/`..` and the reserved `.cog-domain.lock`. Controls fail INVALID_ARG; lengths
over 128 bytes fail OVERFLOW. Descriptor-relative no-follow file/directory opens
prevent symlink traversal. max_sessions holds the stable ownership-domain admission lock through
counting/publication, including owner bootstrap, so distinct session IDs cannot
oversubscribe it. Concurrent admission may return AGAIN; after the winner commits
a full store returns LIMIT and never prunes stored sessions. Capacity failures return LIMIT without publication.

## Executable evidence

`test_session_store` covers public lifecycle/error paths, v2 complete generations,
append, replacement-save refusal, list order, limit rejection without pruning,
opaque embedded-NUL bytes, known usage, explicit v1 migration and exact custom
backend_context/public snapshot restore. `test_store_adversarial` covers exact
old checkpoints, stale CAS/guards, same-process contention, borrow lifetime,
PROPOSED eligibility, recovery-only normalization/reconciliation, replacement
result immutability and corrupt-latest refusal without fallback.

`test_store_fencing` uses real fork+exec children and independently opened locks,
SIGKILL/reap after durable STARTED, fresh-epoch recovery, and anchor inode
stability. Test-only linker wrappers inject EIO and abrupt process exit at all
16 write/fsync/link/unlink/rename boundaries of STARTED->SUCCEEDED+result publication,
and all eleven owner CAS boundaries. Every restart observes exactly complete old
or complete new state, old exact checkpoint remains valid, and identical CAS
retry durably resolves uncertain publication. The same test injects every store
constructor allocation failure and proves zero direct Cog allocations during
save/read/CAS/checkpoint/borrow/release (explicit guard constructor OOM is tested).
The wrappers call real syscalls and are absent from production builds. A
deterministic multiprocess admission race verifies max_sessions across distinct
IDs. A retained-temp hardlink fixture and repeated unlink EIO prove that retries
never truncate a committed checkpoint. Successful or uncertain publication marks
source nodes committed; generic custom-save bookkeeping is conservative on IO,
and reset/discard cannot erase those source nodes, including live occupied slots
beyond holes left by discarded drafts. Parent-directory syscall ordering is
verified for first save and owner bootstrap; injected parent fsync EIO prevents
publication and retry confirms durability through EEXIST. These are filesystem
ordering/failure tests, not a simulated power-loss guarantee.

Seven targeted CMake tests pass under C11 Werror and ASan/UBSan with leak
detection. These are targeted G evidence; unrelated provider/application delivery
and broad package integration are separate gates. Independent review and combined
G/Q integration evidence are recorded below.

## Stage 14.G private codec boundary (implemented)

`session_store_codec.c/.h` encodes public snapshots as explicit little-endian
scalars with an 8-byte `COGSNAP2` version marker, explicit presence/length byte
strings, binary64 bit patterns, complete nodes/messages/opaque/usage and
ledger/audit records. No native structs, padding, pointers or platform indexes
are serialized. The transaction backend wraps this payload in a checksummed
immutable generation; the codec itself neither publishes files nor grants
execution ownership. Constructor-owned input/output and descriptor scratch are
supplied explicitly; encode/decode never allocate or grow memory.

Binary decode retains committed STARTED exactly for monotonic diff comparison.
Public load must pass the decoded snapshot through `cog_session_restore` for
full tree/association/audit validation and conservative INDETERMINATE
normalization. A successful raw decode is not replay admission. Failure leaves
the output snapshot unchanged; scratch/output bytes may have been consumed.

The v1 migration parser accepts the original writer's fixed meta/node schemas,
canonical base64, checked integer/scalar values and valid UTF-8 text. It rejects
duplicate/unknown members, malformed complete records, missing committed
records/bytes and unsupported versions. Optional recovery only discards one
unterminated record beyond the metadata's complete committed record count.
Unidentified legacy tools require INSPECT and never acquire invented call IDs.
Legacy embedded-NUL/nonrepresentable tool strings or invalid UTF-8 text return
UNSUPPORTED without rewriting the source; those old files remain available for
external forensic inspection. No truncation or guessed replay is performed.

`tests/unit/test_session_codec.c` directly verifies exact byte round trips,
opaque embedded NUL, usage, committed STARTED vs restored INDETERMINATE, every
binary truncation boundary, descriptor/encoded capacity failure, unsupported
wire versions, trailing bytes and strict v1 migration/inspection/tail behavior.
Targeted C11-Werror and ASan/UBSan codec/session executions pass. This evidence
does not alone establish atomic manifest transactions or ownership fencing;
the separately reviewed transaction/fencing tests above cover those boundaries.

Additional codec adversarial coverage mutates every byte of a valid structured
record and checks bounded decode/restore without effects, exercises arbitrary
scratch-buffer alignment, duplicate v1 fields, noncanonical base64 padding and
embedded-NUL legacy payload refusal. Scratch allocation aligns actual addresses,
including callers that supply an unaligned byte-buffer start.

## Independent acceptance and combined G/Q integration

Tickets `f94a903`, `953ed5c`, `0a33dd7` and `b8a6fc1` record independent
review, merge and combined validation. Review rejected branch-local result
omission, divergent-draft active selection, a legacy helper regression, sparse
source commit marking and missing store-root parent durability. The accepted
branch also fixes the independently reproduced temporary-hardlink checkpoint
corruption, global session-count admission race and persisted PROPOSED denial.
All reviewed fixes remain in the merge history.

The merged `test_core_service` target links the real `cog-core`, `cog-ipc` and
`cog-obs` archives, including the migrated session/store implementation; it uses
no isolation store double. F transport and Q scheduler CMake registrations are
preserved additively. Twenty-seven targeted tests pass in Debug C11 Werror and
ASan/UBSan with leak detection: the seven G suites, core/service/event/state/
handler/result/tool/IPC/observability units, five core/store integration tests,
and executable scenarios 13/14. Scenario 13 now uses identified assistant calls,
durable STARTED and a correlated final result; scenario 14 preserves shared
ancestor identity and verifies immutable ancestry and independent child metrics.
Legacy core fixtures now zero-initialize complete messages, exclude the text
terminator from UTF-8 byte lengths, provide mandatory custom load_ex and expect
the approved v2 deletion refusal.

Independent reproducers under `/tmp/g2-independent` also pass strict and
sanitizer builds for sparse committed slots, base-parent descriptor syncing,
post-link cleanup EIO/retry, and concurrent max_sessions admission. The permanent
fencing suite covers 16 save and 11 owner CAS syscall boundaries with EIO and
process-exit injection. These checks establish the tested ordering and restart
behavior; they do not simulate a physical power failure.

This is a library delivery gate, not the full application/release gate. The
default full build still stops at the old fake provider vtable's missing poll
initializer in `test_core_provider.c`, owned by H integration. Legacy unit-agent
expectations still assume dynamic results/256 default records rather than Q's
constructor-reserved buffers/16 default slots; release ticket `b569276` tracks
that migration. H/K must finish the real provider/tool conversation loop and
remaining application scenarios. I/J/L/N/O and full installed packaging/release
gate P remain separate; no live or paid model calls were used here.

## Draft v3 instruction context extension

The new `session.h` context/binding APIs and `session_store.h` v3 suffix are
header-first contracts, not implemented by the v2 baseline above. Context IDs,
revision IDs and node associations are immutable. A branch explicitly selects
its recorded or same-leaf pending revision; it cannot inherit a sibling's latest
activation. Reset/discard cannot erase context history or binding anchors.

`COG_STORE_CAP_CONTEXT_V3` is required before instruction-aware load and is
independently checked before any context-bearing save/append/checkpoint callback.
Unknown/unsupported required capabilities fail before callbacks. Custom backend
source must be recompiled for the appended vtable field; binary compatibility
with an old shorter table is not claimed. The whole checkpoint atomically stores
messages, ledger/audit, opaque context bytes, binding revisions and node bindings.
Recovery guards cannot publish contexts or change selection; reconciliation
results retain their originating assistant context.

Tests must cover sparse IDs/high-water, repeated idle publication, sibling and
nonleaf forks, explicit pending-binding selection, inactive leaf discard, blocked
anchor discard/reset, capacity-failure atomicity, malformed/cross-branch restore,
source-free v3 load, v1/v2 legacy zero initialization, unsupported custom callbacks
not invoked, recovery-only refusal, committed-data immutability, and failed-save
freeze. File loads derive context constructor reservations from validated bounded
snapshot records. Existing transaction/fencing tests must remain intact.
