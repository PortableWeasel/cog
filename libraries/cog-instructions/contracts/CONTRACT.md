# Instruction contracts

Public headers are authoritative. This inventory defines implementation and
verification obligations for `tix show 7eb6cf9`; it does not claim implementation
or a passed joined header gate.

## Ownership and dependencies

`cog-instructions` depends on neutral result/prompt/tool descriptor contracts,
never the CLI, provider, network, IPC or runtime. It owns explicit reader,
catalog and snapshot constructor/destructor pairs. Readers are mutable on one
owner thread; catalogs and snapshots are immutable after construction. No global
mutable state, constructors, environment search, shell, dynamic registration or
hidden runtime allocation is allowed. Filesystem traversal belongs exclusively
to the reader. Parser/composer/codec calls operate on supplied bytes.

## Source reader

Types: source root/configuration/limits/request/item/diagnostic, fixed operation
enum, opaque reader. APIs: `reader_new`, `source_begin`, `source_poll`,
`source_view`, `source_error`, `source_cancel`, `reader_free` (all with the
`cog_instruction_` prefix).

Construction copies roots, opens no-follow directories and reserves every job
buffer, path, result descriptor, directory buffer, sort slot and hash workspace.
Poll observes finite work and byte quanta, including path traversal and sorting.
Read/scope content is exact UTF-8 without NUL, with SHA-256 over original bytes.
Scan results contain names only. Optional absence is distinct from empty text.
Scope order is root to target; scans sort by unsigned UTF-8 path bytes.

Tests cover each operation; empty/missing/invalid roots and paths; symlinks and
nonregular candidates; all limit boundaries; invalid UTF-8/NUL; interrupted and
short reads; detected file mutation; sorting across multiple polls; repeated
terminal poll; failed begin preserving previous views; cancellation; consumed
cleanup errors; and zero runtime allocations. Reader diagnostics contain only
logical identity and original source offset, never raw content/absolute paths.

## Catalog and snapshots

Types: source kind/record, constructor limits, entry, selection, tool record,
manifest, restore mode and opaque catalog/snapshot. APIs: `catalog_new`,
`catalog_view`, `snapshot_new`, `snapshot_view`, `snapshot_restore`, `prompt_view`,
`catalog_free`, `snapshot_free` (same prefix).

The bounded metadata grammar and exact fragment rules are in `instructions.h`
and `docs/DESIGN.md`. Source identities are unique. Equal-precedence duplicate
names fail; higher host precedence wins deterministically. Source bodies remain
cached but unselected agent/skill bodies do not enter model context. Frontmatter
is parsed as data, never evaluated. Descriptor bindings come from the host's
original static table. Agent restrictions and skill capability requirements can
only reduce authority. ASK is never promoted to ALLOW. Resource additions stay
inside the active owner's skill package.

Tests cover every scalar style and escape, LF/CRLF, duplicate/unknown fields,
unsupported YAML constructs, UTF-8 character ceilings, filename/name agreement,
precedence and scan-order invariance, role/model mismatch, exact tool subsets,
capability-filtered discovery, activation union/deduplication, resource ownership,
ordering and exact fragment bytes. Constructors must deep-copy all text. Failure
leaves output NULL with valid caller-owned diagnostics; inputs remain unchanged.
Every configured count/byte ceiling and checked arithmetic path is tested.

Restore validates both source hashes and derived metadata/fragments, preserves
archived policy and refuses descriptor/schema/model/template drift. INSPECT has
no executable prompt view. Tests mutate each retained identity/hash/fragment,
try stronger/weaker/extra host bindings, free original owners, and verify the
restored snapshot remains complete without reading files.

## Portable manifest codec

Types/constants: `instruction_codec.h` defines context namespace/format. APIs:
`cog_instruction_manifest_encode`, `cog_instruction_snapshot_decode`,
`cog_instruction_snapshot_fingerprint`. Fingerprints hash the same canonical
encoding without allocating an encoded copy and grant no execution authority.

CGIM v1 uses explicit little-endian integers and counted UTF-8 strings, arrays
and exact source bytes. Native pointers/padding never appear. Encode measurement
and insufficient-output paths do not change output bytes. Decode rejects unknown
versions, truncation, trailing bytes, invalid counts, enum values and duplicate
records before exposing an owner. All allocations are bounded by constructor
limits, never solely by encoded lengths. Semantic restore validation is shared.

Tests use a fixed golden byte vector, empty and rich manifests, round trips,
input-owner destruction, every-byte truncation, crafted count overflow, unknown
versions, inconsistent hashes/fragments, and output canaries. No codec operation
reads files, resolves credentials, invokes a tool or makes a provider request.

## Error and integration gates

Headers enumerate exact NULL_PTR, INVALID_ARG, INVALID_STATE, NOT_FOUND,
DENIED, UNSUPPORTED, PROTOCOL, LIMIT, OVERFLOW, NOMEM, IO, AGAIN and ABORTED
cases. A failure cannot become an empty successful catalog or truncated prompt.

Required gates: strict standalone GCC/Clang C11 and C++17 headers; hermetic
loader-only consumer; all module contract cases; ASan/UBSan; restored-source
integration and exact child-policy reductions. Runtime/store/native tests must
separately prove publication before effects and durable context association.
Passing this library's tests alone does not pass those integration gates.
