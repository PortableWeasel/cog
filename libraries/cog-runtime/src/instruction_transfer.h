/**
 * @file instruction_transfer.h
 * @brief Draft private runtime capsule/resource wire contracts; no UI ownership.
 * Single owner. Pure codecs perform no allocation, I/O, authentication, context
 * publication or effect admission. Outer integers are big-endian; nested CGIM
 * retains its canonical little-endian format from cog/instruction_codec.h.
 */
#ifndef COG_CLI_INSTRUCTION_TRANSFER_H
#define COG_CLI_INSTRUCTION_TRANSFER_H
#include <cog/result.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/** Exact child-specific or checkpoint identity; never execution authority. */
typedef struct cog_cli_instruction_identity {
    uint64_t catalog_generation;
    uint64_t snapshot_id;
    unsigned char sha256[32]; /**< cog_instruction_snapshot_fingerprint result. */
} cog_cli_instruction_identity_t;

/** Borrowed immutable full CGIM. All fields zero together for legacy mode. */
typedef struct cog_cli_instruction_capsule {
    cog_cli_instruction_identity_t identity;
    const void *bytes;
    size_t length;
} cog_cli_instruction_capsule_t;

/** CGB2 view; decoded bodies borrow the caller's output arena. */
typedef struct cog_cli_instruction_bootstrap {
    const void *configuration; /**< Exactly one complete CGB1 body. */
    size_t configuration_bytes;
    cog_cli_instruction_capsule_t instructions;
} cog_cli_instruction_bootstrap_t;

/** @brief Encode complete CGB2 without partial publication.
 * @param[in] bootstrap Immutable CGB1 plus CGIM and identity.
 * @param[out] output Arena, NULL iff capacity0.
 * @param[in] capacity Arena bytes. @param[out] required Exact encoded bytes.
 * @return OK; NULL_PTR; INVALID_ARG shape; LIMIT arithmetic;
 * OVERFLOW changes only required. Other failures change no output. No overlap.
 * Literal: CGB2(4),configuration_length:u32,exact CGB1 bytes,catalog:u64,
 * snapshot:u64,sha256:32,capsule_length:u32,exact CGIM bytes. No trailing bytes.
 * Empty capsule requires NULL bytes and zero identities/digest. Nonempty requires
 * nonzero identities, matching SHA-256 over entire capsule, CGIM magic/version1.
 * Codec validates framing/prefix/hash only; the caller must validate full CGB1
 * and restore CGIM under constructor limits and exact static bindings before
 * use. Identity fields must then equal the restored manifest. Missing capsule
 * is permitted ONLY for explicitly instruction-free runs, never a fallback.
 * Complete body plus CGL envelope must fit trusted frame/profile reservation
 * before admission; oversize is LIMIT, never truncation or protocol downgrade.
 */
cog_result_t cog_cli_instruction_bootstrap_encode(
    const cog_cli_instruction_bootstrap_t *bootstrap, void *output,
    size_t capacity, size_t *required);

/** @brief Copy and frame-check complete CGB2 transactionally.
 * @param[in] input Full body. @param[in] length Exact bytes.
 * @param[out] bootstrap View borrowing storage until overwritten.
 * @param[out] storage Copied CGB1+CGIM bytes, NULL iff capacity0.
 * @param[in] capacity Bytes. @param[out] required Required copied bytes.
 * @return OK; NULL_PTR; PROTOCOL shape/prefix/hash; UNSUPPORTED recognized
 * unknown CGB/CGIM version; LIMIT arithmetic; OVERFLOW changes required only.
 * All other outputs unchanged on failure; nonoverlap. Validate framing before
 * capacity. No inner constructor validation is implied. Caller validates CGB1,
 * restores CGIM and matches identities/model/template/limits/effective tools,
 * then completes exact store probe and provider preflight before PROFILE ACK.
 * Read generation through typed CGB1 decode, never fixed CGB2 offsets.
 */
cog_result_t cog_cli_instruction_bootstrap_decode(const void *input,
    size_t length, cog_cli_instruction_bootstrap_t *bootstrap, void *storage,
    size_t capacity, size_t *required);

/** One independent stop/wait context exchange, retained on RESOURCE lane. */
typedef enum cog_cli_instruction_resource_kind {
    COG_CLI_INSTRUCTION_RESOURCE_REQUEST = 1,
    COG_CLI_INSTRUCTION_RESOURCE_RESULT,
    COG_CLI_INSTRUCTION_RESOURCE_CONSUMED,
    COG_CLI_INSTRUCTION_RESOURCE_COMMIT,
    COG_CLI_INSTRUCTION_RESOURCE_READY
} cog_cli_instruction_resource_kind_t;
/** Explicit replacement reason; no model-selected source/identity authority. */
typedef enum cog_cli_instruction_resource_operation {
    COG_CLI_INSTRUCTION_RESOURCE_LOAD = 1,
    COG_CLI_INSTRUCTION_RESOURCE_ACTIVATE,
    COG_CLI_INSTRUCTION_RESOURCE_SCOPE
} cog_cli_instruction_resource_operation_t;
/** Factual worker disposition, never inferred from transport acceptance. */
typedef enum cog_cli_instruction_commit_outcome {
    COG_CLI_INSTRUCTION_COMMIT_NONE = 0,
    COG_CLI_INSTRUCTION_COMMIT_SAVED,
    COG_CLI_INSTRUCTION_COMMIT_REFUSED,
    COG_CLI_INSTRUCTION_COMMIT_UNCERTAIN,
    COG_CLI_INSTRUCTION_COMMIT_UNCHANGED
} cog_cli_instruction_commit_outcome_t;
typedef struct cog_cli_instruction_resource {
    cog_cli_instruction_resource_kind_t kind;
    cog_cli_instruction_identity_t context; /**< Exact old coordinator binding. */
    cog_cli_instruction_resource_operation_t operation;
    const char *root_id;
    const char *skill_name;
    const char *path;
    uint64_t assistant_node_id; /**< REQUEST: exact durable proposal owner. */
    const char *call_id; /**< REQUEST: exact identified call,1..255 UTF-8 bytes. */
    uint16_t call_ordinal; /**< REQUEST:0..15, matching assistant call array. */
    uint64_t request_checkpoint_generation; /**< REQUEST: exact immutable proof. */
    cog_result_t result; /**< RESULT status only; other kinds require OK. */
    cog_cli_instruction_capsule_t candidate; /**< RESULT owns no native pointers. */
    cog_cli_instruction_commit_outcome_t outcome;
    uint64_t checkpoint_generation; /**< SAVED exact nonzero guarded checkpoint. */
} cog_cli_instruction_resource_t;

/** @brief Encode one context transaction body, no reads/publication/admission.
 * @param[in] resource Immutable typed input. @param[out] output NULL iff capacity0.
 * @param[in] capacity Bytes. @param[out] required Exact bytes.
 * @return OK; NULL_PTR; INVALID_ARG shape/path/hash/status; LIMIT arithmetic;
 * OVERFLOW changes required only. Other outputs unchanged; no overlap.
 *
 * Literal CGIR V2, all outer integers big-endian:
 * Common prefix: CGIR(4),version:u8=2,kind:u8,old_catalog:u64,old_snapshot:u64,
 * old_capsule_sha256:32. Old IDs are nonzero.
 * REQUEST appends operation:u8,root,skill,path (each u32 length + UTF-8,no NUL),
 * assistant_node_id:u64,call_ordinal:u16,call_id(u32 length+UTF-8),
 * request_checkpoint_generation:u64. Node/checkpoint are nonzero, ordinal0..15,
 * call ID1..255 bytes without NUL. The exact saved assistant call at that ordinal
 * must have that ID and original args; no alternative node/branch lookup.
 * LOAD=1 requires all three nonempty; path is package-relative resource path.
 * ACTIVATE=2 requires nonempty root/skill, empty path. SCOPE=3 requires workspace
 * root,empty skill,normalized root-relative directory path (empty means root).
 * All paths reject absolute/dot/dot-dot components/repeated or trailing slash.
 * Root IDs/skill names use the instruction format's exact bounded grammar.
 * RESULT appends status:u8. Status0 appends candidate_catalog:u64,
 * candidate_snapshot:u64,candidate_sha256:32,capsule_length:u32,exact CGIM bytes.
 * Status1=NOT_FOUND,2=DENIED,3=LIMIT,4=IO,5=ABORTED,6=TIMEOUT,7=UNSUPPORTED,
 * 8=PROTOCOL; error result ends after status, candidate entirely zero/NULL.
 * Successful candidate IDs >0, same pinned catalog. A changed candidate has a
 * fresh identity. A no-change candidate MUST have old identity AND byte-identical
 * canonical CGIM; matching hash alone is insufficient. An unchanged SCOPE chain
 * with unchanged scope metadata, already-active ACTIVATE, or identical retained
 * LOAD may return no-change. Changed scope metadata requires fresh identity.
 * Require matching capsule hash/prefix/version. CGIM carries exact resource bytes
 * and hashes; no second resource-text copy or ambiguous independently edited
 * source exists. Full semantic validation belongs to snapshot_decode/restore.
 * COMMIT/READY append candidate_catalog:u64,candidate_snapshot:u64,
 * candidate_sha256:32,outcome:u8,checkpoint_generation:u64. Candidate bytes=NULL,
 * length0; identity equals retained successful RESULT or all-zero for error.
 * Outcomes SAVED=1,REFUSED=2,UNCERTAIN=3,UNCHANGED=4. SAVED requires a changed
 * successful candidate
 * and nonzero exact checkpoint; other outcomes require checkpoint0. UNCHANGED
 * requires successful byte-identical old candidate and exact old identity.
 * UNCERTAIN
 * requires a changed successful candidate; an error RESULT permits only REFUSED.
 * CONSUMED contains ONLY common prefix. READY echoes COMMIT fields exactly.
 * Request call/node/ordinal/checkpoint fields are zero/NULL on every other kind;
 * replies bind them through exact retained REQUEST bytes and outer reply_to.
 * Unused operation/strings/result/candidate/outcome/checkpoint fields are
 * zero/NULL (result OK). REQUEST/RESULT use outcome NONE/checkpoint0.
 * No native error values, padding, trailing fields or unknown extension.
 */
cog_result_t cog_cli_instruction_resource_encode(
    const cog_cli_instruction_resource_t *resource, void *output,
    size_t capacity, size_t *required);

/** @brief Validate/copy complete CGIR V2 without performing its operation.
 * @param[in] input Full body. @param[in] length Exact bytes.
 * @param[out] resource View borrowing storage until overwritten.
 * @param[out] storage Copied strings (with NUL) and capsule,NULL iff capacity0.
 * @param[in] capacity Bytes. @param[out] required Exact copied bytes.
 * @return OK; NULL_PTR; PROTOCOL shape/kind/status/path/UTF-8/hash;
 * UNSUPPORTED recognized version other than2; LIMIT arithmetic;
 * OVERFLOW changes required only. Other outputs unchanged. Validate full frame
 * before capacity; nonoverlap. No allocation,I/O,CGIM semantic restore or grant.
 */
cog_result_t cog_cli_instruction_resource_decode(const void *input,
    size_t length, cog_cli_instruction_resource_t *resource, void *storage,
    size_t capacity, size_t *required);

/** Context transaction lifecycle; one operation per executor on RESOURCE lane:
 * Reserve original/result/commit/ready/consumed arenas and wire IDs BEFORE
 * request admission. Entire frames fit trusted frame/capsule reservations, no
 * chunk protocol, truncation or growth. Authenticate actual fabric/peer/kernel
 * proof/managed incarnation plus CGL runtime/session/attempt/placement, then
 * exact old context. IDs come from runtime, never model-selected alternatives.
 * Coordinator owns canonical association and per-session/catalog ID high-water
 * derived from ALL durable context history, not selected branch alone. Reserve
 * candidates monotonically in live owner; never reissue a published or uncertain
 * ID. A new child session has its own domain, bound by exact CGL session/attempt.
 * Abandoned unpublished IDs may burn live; they never admit effects.
 * No worker independently publishes a replacement or allocates snapshot IDs.
 *
 * REQUEST worker->coordinator reply_to0. Coordinator first INSPECT-loads exact
 * request_checkpoint_generation under the known current owner/session, checks
 * assistant_node_id/call_ordinal/call_id and original args, and matches the node's
 * immutable instruction binding to the request old context. SCOPE must name the
 * PROPOSED workspace call's actual target directory (command declared cwd), never
 * an arbitrary model claim. LOAD/ACTIVATE must name the exact STARTED runtime
 * control descriptor and requested args, with persisted STARTED evidence. This
 * one requesting control is the only permitted STARTED exception: its bytes and
 * effect identity remain old-bound until ordinary terminal closure. Any other
 * STARTED/indeterminate effect refuses replacement. Record this proof and request
 * immutably before preparing a candidate; latest-checkpoint guessing is forbidden.
 * LOAD verifies active skill/root in old
 * pinned catalog, reads beneath its package using B1 no-follow, and joins its
 * package-relative path to the pinned SKILL.md parent for root-relative source
 * identity. ACTIVATE selects an eligible pinned skill and unions activation with
 * existing active set, no file reread. SCOPE performs B1 SCOPE at the declared
 * workspace directory. Worker has no instruction source roots/read authority.
 * Rebuild from old cached HOST/AGENT/SKILL, retain selected profile/effective
 * policy/model/template and prior active skills/resources, replace only the
 * affected resource/scope chain or activation. No permission/limit increase.
 * Changed resource replacement needs a new snapshot; historical bytes remain.
 * Coordinator compares complete proposed canonical manifest with old snapshot.
 * Unchanged content/settings return exact old CGIM/identity; changed content or
 * scope metadata gets fresh host identity and retained exact canonical candidate.
 * Result is stable even if files subsequently change; no include/script work.
 *
 * RESULT coordinator->worker reply_to exact REQUEST. Worker restores candidate
 * with actual static descriptors/template and checks it against requested change
 * and old identity. Pure preflight and all context/node/ledger/capacity checks
 * precede mutation. Then close stale/unstarted proposed group with identified
 * no-effect denials, publish binding and save result/ledger/context atomically.
 * Before closing any group, compare candidate full bytes and identity to old.
 * Exact no-change bypasses group denial/context mutation/save and yields
 * UNCHANGED. It does not restart/replay a call. Changed candidate follows the
 * group-closing/publication path; denied old call identity is never executed.
 * A different target scope with changed metadata requires one replacement;
 * probing that same unchanged target again returns no-change, not an endless
 * sequence of denials and equivalent snapshot publications.
 * Already STARTED effects retain old context; requests requiring replacement
 * wait/refuse while other such effects or indeterminate outcomes exist. The
 * exact requesting LOAD/ACTIVATE control may already be STARTED: finalize its
 * identified terminal result/ledger under old binding, deny remaining unstarted
 * stale proposals, then atomically save those outcomes with replacement context.
 * No unrelated tool gains a STARTED exception or changes its admitted context.
 *
 * COMMIT worker->coordinator reply_to exact RESULT, with SAVED and exact checked
 * guarded checkpoint, REFUSED only for pre-mutation/preflight/no-candidate
 * rejection, UNCHANGED for exact byte-identical old context with checkpoint0,
 * or UNCERTAIN for any failure after in-memory mutation/save attempt.
 * Worker retains candidate/evidence and freezes all admission after UNCERTAIN;
 * it never rolls back old identity or retries an effect. Coordinator validates
 * checkpoint through shared-store INSPECT, exact session/owner/selected context
 * and full CGIM bytes before accepting SAVED. Saved checkpoint generation must
 * exceed request proof. It must preserve exact request call identity/args and
 * old binding, include the requesting LOAD/ACTIVATE terminal result/ledger, and
 * contain identified no-effect DENIED outcomes for every still-unstarted stale
 * proposal in that assistant group (SCOPE includes its requesting call). Earlier
 * factual terminal outcomes are preserved; no STARTED/uncertain call is rewritten
 * as a denial. No unresolved group remains before context publication. Verify
 * these closure facts together with selected new context in the SAME checkpoint.
 * A context-only save or receipt is insufficient. Any contradictory proof freezes
 * both ends; no successful READY may be fabricated from a transport send.
 * Only accepted SAVED updates coordinator association from old to candidate.
 * UNCHANGED is accepted only against retained old full CGIM, with no store write
 * or changed association. It is not a shortcut for a failed replacement save.
 * READY coordinator->worker reply_to exact COMMIT echoes factual disposition.
 * Worker holds all new effects/model sends/context requests until exact READY;
 * SAVED then activates saved candidate, REFUSED retains old context but never
 * authorizes a stale scope proposal; ordinary no-effect refusal/result handling
 * must close that proposal and checkpoint before future admission,
 * UNCERTAIN stays frozen. UNCHANGED READY releases only this probe gate; the
 * original unstarted call may then proceed through ordinary policy/schema/limit
 * checks and guarded STARTED exactly once. No effect occurs from READY itself.
 * READY for failure permits retirement, never admission.
 * CONSUMED worker->coordinator reply_to exact READY, using original old identity.
 * Coordinator retires transaction only after its own CONSUMED; worker retains
 * exact CONSUMED until copied-send. No recursive ACK. Ordered same-connection
 * delivery lets next request follow consumed, never substitutes for it.
 *
 * At every phase retain exact original/candidate and last reply. Same-ID changed
 * bytes fail PROTOCOL; identical duplicates replay retained reply without source
 * reads, allocation, checkpoint save, context publication or effect repetition.
 * Matching pending correlations are resolved before current-context/watermark
 * checks, so old-bound duplicates remain answerable after accepted SAVED.
 * Unknown retired requests remain stale. PROFILE/UPDATE/HANDOFF never consume
 * RESOURCE proofs. Slot reuse requires this lane AND existing lane retirement.
 * Whole deadline=min(parent/turn deadline,configured finite transaction timeout),
 * unchanged by retries. Timeout/cancel before worker mutation yields retained
 * failure/refusal; after mutation or unknown remote outcome freezes admission
 * and preserves factual state/proofs, never guesses rollback. Successful save
 * wins later cancellation but still requires READY before any admission. On
 * native loss no reconnect/rebind/task reconstruction; retain durable evidence
 * and drain/reap. Native teardown can retire transport proofs only after actual
 * process death/cleanup, never authorize replay. Wipe body/capsule/source arenas
 * after borrows end on failure/free. No bytes enter generic UI/replay/log caches.
 * UNCHANGED skips context publication and group invalidation only. A SCOPE
 * probe is not a model tool result and needs no new checkpoint. Already-active
 * ACTIVATE or byte-identical LOAD are still identified tool calls: after READY,
 * finalize their ordinary terminal result/ledger and guarded checkpoint through
 * the existing tool path before model continuation. Never repeat STARTED or
 * leave the tool unresolved merely because context bytes did not change.
 * Request-proof inspection uses load_report.source_audit_high_water: a last
 * STARTED->INDETERMINATE transition whose sequence exceeds that source watermark
 * is restore's synthetic normalization, not preexisting persisted uncertainty.
 * Verify exact prior STARTED audit and request checkpoint; an INDETERMINATE
 * transition at/below the source watermark always refuses ordinary work. This
 * factual distinction validates the trusted live request only, never authorizes
 * an offline inspected session to execute or replay an effect.
 * Local execution uses same preparation/atomic checkpoint semantics with direct
 * operation returns replacing wire ACKs; no second source-resolution policy.
 */
#ifdef __cplusplus
}
#endif
#endif
