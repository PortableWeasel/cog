/**
 * @file session_store.h
 * @brief Pluggable session persistence backend.
 *
 * Defines the interface for long-term storage of conversation sessions.
 * Sessions are trees of nodes with messages, timing, billing, and provider
 * data. The store is responsible for persisting and loading sessions.
 *
 * Version 2 writes structured messages, exact opaque bytes, known/unknown
 * usage and tool execution ledger. v1 text-only sessions migrate losslessly in
 * memory; v1 tool records lack stable call IDs and are inspection-only, never
 * assigned invented IDs or executed/replayed. Unknown versions fail UNSUPPORTED.
 * No on-disk rewrite during load; explicit save publishes v2 atomically.
 * The backend is selected via a static dispatch table. The default
 * implementation is file-based (JSONL + index).
 *
 * Threading model: a store is owned by exactly one core instance and is
 * not thread-safe. All access happens on the thread that runs
 * @c cog_core_run.
 */
#ifndef COG_SESSION_STORE_H
#define COG_SESSION_STORE_H

#include <cog/result.h>
#include <cog/session.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Current structured store version; v1 remains a legacy read format. */
#define COG_STORE_FORMAT_V2 2u
/** @brief Draft context-bearing snapshot format; v1/v2 remain readable. */
#define COG_STORE_FORMAT_V3 3u
/** @brief Static promise: exact context preservation and atomic v3 saves. */
#define COG_STORE_CAP_CONTEXT_V3 UINT64_C(1)

/**
 * @brief Literal portable v3 snapshot suffix (draft before implementation).
 *
 * Magic is eight bytes COGSNAP3. Following bytes through end of ledger use the
 * exact COGSNAP2 sequence; node records here do NOT include the new binding.
 * Append this suffix in order; u64 is unsigned little-endian, eight bytes:
 * - u64 context_high_water, u64 context_binding_high_water,
 *   u64 selected_context_binding_id, u64 context_count;
 * - each context in increasing ID order: u64 context_id, string namespace_id,
 *   u64 format (1..UINT32_MAX), blob data;
 * - u64 context_binding_count, each binding in increasing ID order:
 *   u64 binding_id, u64 previous_binding_id, u64 anchor_node_id, u64 context_id;
 * - u64 node_binding_count (equals node_count), then in original node order:
 *   u64 node_id, u64 context_binding_id.
 * string/blob use existing primitives: presence byte 0 or 1; absent has no
 * following bytes; present has u64 byte length, exact bytes, one zero byte.
 * Strings have no interior NUL. Empty context data MUST use absent encoding;
 * namespace string MUST be present/nonempty. No padding/native structs/pointers,
 * trailing bytes or guessed missing fields. Checked lengths/counts fit size_t,
 * reserved storage and input. Truncation/malformed input PROTOCOL; unknown
 * version UNSUPPORTED; reserved-space exhaustion LIMIT. Validate associations.
 *
 * Generation framing stays COGGEN2 followed by NUL (8 bytes), u64 generation,
 * u64 payload size, u64 payload checksum, u64 header checksum. This existing
 * 40-byte envelope is version-neutral framing; checksum covers the full v3
 * payload. Owner/manifest/checkpoint transaction stays unchanged. Saves with
 * any context history/high-water use v3; context-free saves may remain v2.
 * File load derives explicit context record/binding/byte reservations from the
 * validated bounded snapshot before calling session_restore; default zero
 * context capacity is not used for a v3 context-bearing load. Load remains an
 * explicit constructor operation within max_record_bytes.
 * v1/v2 decode initializes all new fields/node bindings to zero. Load never
 * adopts, scans sources, adds prompts or rewrites disk. Generic core preserves
 * opaque namespace data; instruction runtime requires a supported format or
 * explicit safe legacy adoption, never implicit pending-call rebase.
 */
/** @brief File load policy: unsafe legacy records require explicit inspection. */
typedef enum cog_store_load_mode {
    COG_STORE_LOAD_REPLAY = 0,
    COG_STORE_LOAD_INSPECT
} cog_store_load_mode_t;
/** @brief Facts returned by version-aware load; never stored credentials. */
typedef struct cog_store_load_report {
    uint32_t source_version;
    uint64_t checkpoint_generation; /**< Exact v2/v3 immutable generation; 0 for v1. */
    bool migrated_in_memory;
    bool recovered_truncated_tail;
    size_t discarded_tail_bytes;
    bool replay_safe;
    size_t indeterminate_tools;
    bool contexts_present; /**< Exact context history was loaded. */
    uint64_t source_audit_high_water; /**< Exact encoded high-water BEFORE restore
       appends conservative STARTED->INDETERMINATE recovery transitions. A typed
       inspection verifier may distinguish that synthetic tail by sequence above
       this watermark and exact previous/next states; it never grants execution.
       Context-capable custom backends must report this factual source value. */
} cog_store_load_report_t;

/** @brief Immutable checkpoint identity scoped by store+session; 0 is absent. */
typedef struct cog_store_checkpoint { uint64_t generation; } cog_store_checkpoint_t;
/** @brief Durable execution owner, independent of ephemeral socket peer IDs. */
typedef struct cog_store_owner {
    uint64_t epoch; /**< Monotonic, checked increment; 0 only for absent owner. */
    uint64_t instance_id; /**< Nonzero process incarnation, never reused on restart. */
} cog_store_owner_t;
/** @brief Explicit constructor-owned same-host execution guard. */
typedef struct cog_store_owner_guard cog_store_owner_guard_t;

/* ------------------------------------------------------------------ */
/* Opaque types                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Opaque session store.
 *
 * Created with @c cog_store_new and destroyed with @c cog_store_free.
 */
typedef struct cog_store cog_store_t;

/* ------------------------------------------------------------------ */
/* Store operations (vtable)                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Save a session to the store.
 *
 * @param[in] store   Store instance.
 * @param[in] session Session to save.
 * @return COG_OK on success.
 * @note Backend calls obey the full public save monotonic-history contract,
 * including direct callback use; no custom backend may reset effect identity.
 */
typedef cog_result_t (*cog_store_save_fn)(void *store,
                                           const cog_session_t *session);

/**
 * @brief Load a session from the store.
 *
 * @param[in]  store   Store instance.
 * @param[in]  id      Session id to load.
 * @param[out] out     Receives the loaded session.
 * @return COG_OK on success.
 * @return COG_ERROR_NOT_FOUND if the session doesn't exist.
 */
typedef cog_result_t (*cog_store_load_fn)(void *store,
                                           const char *id,
                                           cog_session_t **out);

/**
 * @brief Append a single node to a persisted session.
 *
 * Incremental save — called after each exchange.
 *
 * @param[in] store   Store instance.
 * @param[in] session Session containing the node.
 * @param[in] node    Node to append.
 * @return COG_OK on success.
 */
typedef cog_result_t (*cog_store_append_fn)(void *store,
                                             const cog_session_t *session,
                                             const cog_session_node_t *node);

/**
 * @brief Delete a session from the store.
 *
 * @param[in] store Store instance.
 * @param[in] id    Session id to delete.
 * @return COG_ERROR_UNSUPPORTED for any durable owner record or ownership lock
 * anchor, including after all guards release. This rule also binds delete_;
 * require_owner=false cannot bypass it. Initial profile never unlinks/replaces
 * an ownership anchor or downgrades an owned identity to bootstrap-absent.
 * The refusal check/deletion must serialize with owner bootstrap/adoption in
 * the same stable ownership domain; it cannot race creation of an owner/anchor.
 * @return COG_OK only for supported unowned legacy deletion.
 * @return COG_ERROR_NOT_FOUND if the session doesn't exist.
 */
typedef cog_result_t (*cog_store_delete_fn)(void *store, const char *id);

/**
 * @brief List all session ids in the store.
 *
 * @param[in]  store     Store instance.
 * @param[out] out_ids   Receives array of session ids (caller frees).
 * @param[out] out_count Receives number of sessions.
 * @return COG_OK on success.
 */
typedef cog_result_t (*cog_store_list_fn)(void *store,
                                           char ***out_ids,
                                           size_t *out_count);

/**
 * @brief Check if a session exists in the store.
 *
 * @param[in] store Store instance.
 * @param[in] id    Session id to check.
 * @return true if the session exists.
 */
typedef bool (*cog_store_exists_fn)(void *store, const char *id);

/* ------------------------------------------------------------------ */
/* Store dispatch table                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Session store backend dispatch table.
 *
 * Static const dispatch table — no dynamic registration. For a custom backend,
 * every void *store callback argument is exactly config.backend_context, never
 * a private cog_store_t layout. Caller constructs/destroys that context and
 * keeps it live through store_free and every guard. Builtins use their own
 * explicit store-constructor-owned context; no global custom-backend state.
 * Custom save/checkpoint callbacks obtain complete public data through
 * cog_session_snapshot_view; load/load_ex/checkpoint_load construct it through
 * cog_session_restore. Explicit sparse IDs, selected branch, ledger/audit and
 * committed high-water survive without private layout or dummy-node allocation.
 * Required save/load/append/delete_/list/exists/load_ex pointers are validated
 * before constructor effects. Fenced callbacks form one complete optional group;
 * require_owner rejects a missing member with UNSUPPORTED.
 */
typedef struct cog_store_vtable {
    const char *name;
    cog_store_save_fn save;
    cog_store_load_fn load;
    cog_store_append_fn append;
    cog_store_delete_fn delete_;
    cog_store_list_fn list;
    cog_store_exists_fn exists;
    /** Version-aware load, same error/ownership rules as cog_store_load_ex.
     * Mandatory for v2 backends; old custom tables return UNSUPPORTED until
     * upgraded rather than losing structured fields. */
    cog_result_t (*load_ex)(void *store, const char *id, cog_store_load_mode_t mode,
                            cog_session_t **out, cog_store_load_report_t *report);
    /** Optional fenced-checkpoint profile. All must exist when require_owner;
     * backend handle acquire/release is an explicit constructor pair, never
     * exposes private wrapper layout. Errors/lifetimes match public operations. */
    cog_result_t (*checkpoint_save)(void *store, const cog_session_t *session,
                                    const void *guard, cog_store_checkpoint_t *out);
    cog_result_t (*checkpoint_load)(void *store, const char *id,
                                    cog_store_checkpoint_t checkpoint,
                                    cog_store_load_mode_t mode, cog_session_t **out,
                                    cog_store_load_report_t *report);
    cog_result_t (*owner_read)(void *store, const char *id,
                               cog_store_owner_t *owner, cog_store_checkpoint_t *checkpoint);
    cog_result_t (*owner_compare_exchange)(void *store, const char *id,
                                           cog_store_owner_t expected,
                                           cog_store_owner_t next,
                                           cog_store_checkpoint_t checkpoint);
    /** Normal and recovery acquire callbacks are invoked only AFTER public
     * wrapper/bookkeeping storage is reserved. Reservation NOMEM invokes neither
     * callback and leaves public output NULL. Callback failure leaves backend
     * output NULL with no retained handle/resource; callback success returns one
     * owned handle. No fallible wrapper allocation follows successful callback,
     * so constructor rollback never relies on a possibly refused owner_release.
     * Applies identically to owner_acquire_recovery below. */
    cog_result_t (*owner_acquire)(void *store, const char *id,
                                  cog_store_owner_t expected, void **out_guard);
    /** Release an admitted backend guard; exact public owner_release lifetime.
     * With valid original context/handle, INVALID_STATE is the ONLY
     * nonconsuming refusal: keep handle and all remaining resources intact.
     * OK or any cleanup error (including IO) consumes the backend handle once;
     * perform best-effort resource teardown, never retain a retryable handle.
     * No asynchronous AGAIN ownership: a non-INVALID_STATE error is consuming.
     * Public wrapper consumes its own guard on the same result classification.
     * Consuming failure is not proof that ownership transfer/release succeeded.
     * Custom fenced backends must implement this rule; no layout/version change
     * can adapt an older retain-on-error callback automatically. */
    cog_result_t (*owner_release)(void *store, void *guard);
    /** Restricted recovery guard; same explicit ownership/locking as acquire.
     * Required with the fenced profile; never authorizes execution borrows. */
    cog_result_t (*owner_acquire_recovery)(void *store, const char *id,
                                           cog_store_owner_t expected,
                                           void **out_guard);

    /** Static bits: zero is legacy v2; CONTEXT_V3 promises exact data/bindings/
     * node associations/high-water on ALL load/save/checkpoint paths, atomic
     * with messages/ledger. Unknown bits are rejected at construction. This
     * layout extension requires custom backend recompilation; no binary ABI
     * compatibility with a shorter pre-extension table is promised. */
    uint64_t capabilities;
} cog_store_vtable_t;

/* ------------------------------------------------------------------ */
/* Store configuration                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Configuration for a session store.
 */
typedef struct cog_store_config {
    /**
     * @brief Backend vtable. If NULL, uses the default file backend.
     */
    const cog_store_vtable_t *backend;
    /** Borrowed custom-backend state, passed verbatim to its callbacks. May be
     * NULL for a stateless custom backend. Ignored for builtin file/null tables;
     * those contexts are constructor-owned. Never freed by generic store_free. */
    void *backend_context;

    /**
     * @brief Base directory for file-based backends.
     *
     * Sessions are stored as subdirectories: <base_dir>/<session_id>/
     */
    const char *base_dir;

    /**
     * @brief Maximum number of sessions to keep.
     *
     * Zero means no count policy. At a nonzero limit a new session save
     * returns LIMIT; no stored session is silently deleted.
     */
    size_t max_sessions;

    /**
     * @brief Whether to auto-save after each exchange.
     */
    bool auto_save;

    /**
     * @brief Whether to compress stored data.
     */
    bool compress;
    /** Recover only one unterminated final JSONL record after a fully valid
     * committed prefix. Recovery may discard ONLY bytes beyond the manifest's
     * committed record count/checksum (v1: validated metadata count). A missing
     * byte/record inside that prefix always fails IO; never drop a committed
     * result or ledger intent. Default false (IO on any truncation). A malformed
     * complete line, invalid association, bad checksum/version/index metadata
     * or middle corruption always fails IO/PROTOCOL; never skip records. */
    bool recover_truncated_tail;
    /** Constructor-reserved read/write/codec workspace; 0 selects 16 MiB.
     * Above capacity returns LIMIT, never hidden growth or partial publication. */
    size_t max_record_bytes;
    bool require_owner; /**< Same-host fenced execution profile. Legacy save/append
       consult this context's live guard and return INVALID_STATE without one;
       unguarded APIs cannot bypass ownership. A recovery-only guard permits
       only explicit reconciliation writes, never ordinary append/execution. File backend supports it; null or
       incapable custom backend returns UNSUPPORTED at construction. */
    /** Required static bits checked before backend callbacks. Unknown or
     * unsupported bits return UNSUPPORTED at construction. Instruction runtime
     * requires CONTEXT_V3 before load/adoption, preventing legacy callbacks
     * from silently stripping context before validation. */
    uint64_t required_capabilities;
} cog_store_config_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Create a new session store.
 *
 * @param[out] out    Uninitialised pointer; set to the new store on success.
 * @param[in]  config Configuration; may be NULL for defaults (file backend
 *                    in current directory).
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p out is NULL.
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * @post On success, @p *out is a valid store and the caller owns it.
 * @post On failure, @p *out is NULL and no resources are held.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_store_new(cog_store_t **out,
                            const cog_store_config_t *config);

/**
 * @brief Destroy a session store.
 *
 * @param[in,out] store Store to destroy; may be NULL (no-op).
 * @pre All owner guards have been released and every guard borrower/execution
 * is drained and destroyed. Guards retain this context/backend lifetime; free
 * never invalidates a live guard or implicitly releases its ownership anchor.
 *
 * @post All resources held by @p store are released.
 *
 * Threading: not thread-safe.
 */
void cog_store_free(cog_store_t *store);

/* ------------------------------------------------------------------ */
/* Operations (dispatch to backend)                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Durably publish a complete v2 session generation.
 *
 * All structured messages, opaque byte lengths/bytes, usage availability and
 * tool ledger are saved together. Write temporary files, fsync files, atomically
 * publish a generation manifest, then fsync the directory; readers observe the
 * complete old or complete new generation. Replacing nodes/index separately
 * without an atomic generation commit is insufficient. Save failure before
 * publication leaves the old generation; a failed final durability sync returns
 * IO and caller treats effects as uncertain, never authorizing tool execution.
 * No credential/raw authorization bytes may be persisted. In require_owner
 * mode this context MUST hold the live matching session guard; otherwise
 * INVALID_STATE, including legacy save/append and auto-save paths. Normal saves
 * preserve current owner and advance immutable checkpoint generation.
 * Every normal save/append (including backend and auto-save paths) compares
 * against the validated locked committed generation before publication. Preserve
 * each committed identified call, its session/assistant/node/ancestor binding,
 * call ID/name/arguments/opaque data, associated result and all prior audit
 * evidence; no removal, rebinding, result replacement or identity high-water
 * rollback. This is authoritative even for a newly constructed replacement
 * session using the same ID, or after local discard/reset. New node IDs must be
 * beyond the committed high-water; unchanged existing identities remain bound.
 * Context records/bindings and node binding identities are also immutable:
 * no removal, byte replacement, association rewrite, ID reuse or high-water
 * rollback. New IDs exceed committed high-water. Recovery guards cannot add
 * contexts/bindings or change the selected cursor; reconciliation results
 * inherit their identified assistant binding. Ordinary publication may select
 * a safe historical branch or append a valid idle revision after all proposed
 * groups close. When a previously committed unresolved group remains unresolved,
 * no new binding or selected-cursor change is permitted. Publishing before a
 * new group is proposed, or after the old group fully closes, remains valid
 * when all snapshot associations validate. Save the whole snapshot in one generation, never a sidecar.
 * Public save/append/checkpoint reject UNSUPPORTED BEFORE backend callbacks if
 * any context records/bindings/high-water/cursor/node bindings exist but backend
 * lacks CONTEXT_V3, regardless of configured required_capabilities. Failed or
 * uncertain save grants no use of the new context.
 * Only documented monotonic ledger transitions are allowed: PROPOSED->STARTED/
 * DENIED and STARTED->SUCCEEDED/FAILED/CANCELLED/INDETERMINATE, with valid
 * associated results; final outcomes/results are immutable. Only a recovery
 * guard may close INDETERMINATE through reconcile_tool. Append unrelated work
 * only when session execution/associations are safe; INDETERMINATE immediately
 * halts new model/tool/child/input admission even under an already held guard.
 * Known or uncertain publication failure never makes prior intent/evidence
 * erasable. Reject invariant violations INVALID_STATE/PROTOCOL without publishing.
 * A context
 * without require_owner cannot modify any already-owned session (INVALID_STATE).
 *
 * @param[in] store   Store instance.
 * @param[in] session Session to save.
 * @return COG_OK; NULL_PTR; NOMEM for backend setup; LIMIT; IO; PROTOCOL for
 * invalid session associations; UNSUPPORTED for backend version/capabilities.
 * @note Runtime Cog work uses constructor storage, no implicit buffer growth.
 * Threading: not thread-safe.
 */
cog_result_t cog_store_save(cog_store_t *store,
                             const cog_session_t *session);

/**
 * @brief Load replay-safe v1 text or v2; equivalent to load_ex(REPLAY).
 *
 * Unsafe legacy tool identity fails UNSUPPORTED with *out=NULL. Outstanding
 * STARTED v2 entries become INDETERMINATE in memory; never rerun. The caller
 * inspects/reconciles them before continuation; ordinary history building fails.
 *
 * @param[in]  store Store instance.
 * @param[in]  id    Session id to load.
 * @param[out] out   Receives the loaded session.
 * @return COG_OK on success.
 * @return COG_ERROR_NOT_FOUND if the session doesn't exist.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_store_load(cog_store_t *store,
                             const char *id,
                             cog_session_t **out);

/**
 * @brief Version-aware load with explicit recovery/inspection evidence.
 * @param[in,out] store Store; single owner thread, no concurrent writer.
 * @param[in] id Nonempty session ID (no traversal/separators).
 * @param[in] mode REPLAY rejects unsafe legacy tool records; INSPECT loads them
 * with report.replay_safe=false and disables send/tool history reconstruction.
 * @param[out] out Owned session, matched by cog_session_free; NULL on failure.
 * @param[out] report Required evidence; unchanged on failure.
 * @return COG_OK; NULL_PTR; INVALID_ARG; NOT_FOUND; UNSUPPORTED for version or
 * unsafe REPLAY migration; LIMIT/NOMEM; IO/PROTOCOL for corruption. Missing
 * index may be rebuilt from the validated log in reserved workspace; a corrupt
 * authoritative generation is never silently replaced with another branch.
 * @note REPLAY validates that all unresolved tool/result state lies on the
 * one active path; off-active unresolved state fails PROTOCOL, never silent
 * branch selection or dropped evidence. INSPECT grants no execution/recovery
 * bypass. Recovery is enabled only by recover_truncated_tail; no disk writes.
 * Exact opaque data and call associations survive save/load/fork round trips.
 */
cog_result_t cog_store_load_ex(cog_store_t *store, const char *id,
                               cog_store_load_mode_t mode, cog_session_t **out,
                               cog_store_load_report_t *report);

/**
 * @brief Commit one node and its execution ledger without replaying effects.
 *
 * Uses the same durable generation transaction as save; cannot expose a result
 * without its final ledger state. Existing-node/ledger updates require save.
 * A v1 store must first be explicitly saved as v2; append returns UNSUPPORTED.
 *
 * @param[in] store   Store instance.
 * @param[in] session Session containing the node.
 * @param[in] node    Node to append.
 * @return COG_OK on success.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_store_append(cog_store_t *store,
                               const cog_session_t *session,
                               const cog_session_node_t *node);

/**
 * @brief Delete a session.
 *
 * @param[in] store Store instance.
 * @param[in] id    Session id to delete.
 * @return COG_ERROR_UNSUPPORTED for any durable owner record or ownership lock
 * anchor, including after all guards release. This rule also binds delete_;
 * require_owner=false cannot bypass it. Initial profile never unlinks/replaces
 * an ownership anchor or downgrades an owned identity to bootstrap-absent.
 * The refusal check/deletion must serialize with owner bootstrap/adoption in
 * the same stable ownership domain; it cannot race creation of an owner/anchor.
 * @return COG_OK only for supported unowned legacy deletion.
 * @return COG_ERROR_NOT_FOUND if the session doesn't exist.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_store_delete(cog_store_t *store, const char *id);

/**
 * @brief List all session ids.
 *
 * @param[in]  store     Store instance.
 * @param[out] out_ids   Receives array of session ids.
 * @param[out] out_count Receives number of sessions.
 * @return COG_OK on success.
 *
 * @note The caller must free each id string and the array.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_store_list(cog_store_t *store,
                             char ***out_ids,
                             size_t *out_count);

/**
 * @brief Check if a session exists.
 *
 * @param[in] store Store instance.
 * @param[in] id    Session id to check.
 * @return true if the session exists.
 *
 * Threading: not thread-safe.
 */
bool cog_store_exists(cog_store_t *store, const char *id);

/**
 * @brief Return true if the store is configured to auto-save.
 *
 * @param[in] store Store to query; may be NULL.
 * @return true if @p store is non-NULL and its auto_save flag is enabled;
 *         false otherwise.
 *
 * Threading: not thread-safe.
 */
bool cog_store_auto_save(const cog_store_t *store);

/**
 * @brief Save an exact immutable checkpoint under a live matching owner guard.
 * @param[in,out] store Store with require_owner enabled.
 * @param[in] session Quiescent complete replay-safe session, no active handles
 * or unresolved effects. @param[in] guard Live guard for this session/context.
 * @param[out] checkpoint Exact newly committed generation, unchanged on failure.
 * @return OK; NULL_PTR; INVALID_STATE stale/mismatched guard; INDETERMINATE
 * unresolved effects; LIMIT/IO/UNSUPPORTED. Same generation durability rules
 * as save; failure never authorizes activation or rollback based on assumption.
 * @note Guard remains held on return; no runtime workspace growth.
 */
cog_result_t cog_store_checkpoint_save(cog_store_t *store,
                                        const cog_session_t *session,
                                        const cog_store_owner_guard_t *guard,
                                        cog_store_checkpoint_t *checkpoint);
/**
 * @brief Load exactly one immutable checkpoint; never substitute latest.
 * @param[in,out] store Store. @param[in] id Session ID.
 * @param[in] checkpoint Nonzero generation. @param[in] mode REPLAY/INSPECT.
 * @param[out] out Owned session, NULL on failure. @param[out] report Load facts.
 * @return Same load_ex errors, NOT_FOUND for absent generation/zero token;
 * no disk writes, activation, effects or ordinary replay of unresolved history.
 * @note Target loads STAGED while source is quiescent; exact bytes/ledger checked.
 */
cog_result_t cog_store_checkpoint_load(cog_store_t *store, const char *id,
                                        cog_store_checkpoint_t checkpoint,
                                        cog_store_load_mode_t mode,
                                        cog_session_t **out,
                                        cog_store_load_report_t *report);
/**
 * @brief Read committed owner/checkpoint for diagnostics, not an activation grant.
 * @param[in,out] store Store. @param[in] id Session ID.
 * @param[out] owner Current tuple. @param[out] checkpoint Latest immutable data.
 * @return OK (both zero if absent); NULL_PTR; IO; PROTOCOL; UNSUPPORTED.
 * Outputs unchanged on failure. Owner publication and checkpoint are one
 * checksummed/fsynced local manifest; corrupt state never resets to absent.
 */
cog_result_t cog_store_owner_read(cog_store_t *store, const char *id,
                                   cog_store_owner_t *owner,
                                   cog_store_checkpoint_t *checkpoint);
/**
 * @brief Durably compare/exchange quiescent ownership, never execute a task.
 * @param[in,out] store Store. @param[in] id Session ID.
 * @param[in] expected Exact old tuple (0,0 only for bootstrap).
 * @param[in] next Nonzero instance and epoch=expected.epoch+1, no wrap.
 * @param[in] checkpoint MUST equal current committed data generation; no rollback.
 * @return OK; AGAIN while any guard owns the same-host session lock;
 * INVALID_STATE stale owner/checkpoint; NULL_PTR; INVALID_ARG; UNSUPPORTED;
 * INDETERMINATE on uncertain publish/fsync; IO on known prepublication failure.
 * CAS takes the same exclusive independent-open-description lock as acquire,
 * atomically/fsyncs owner+checkpoint manifest, then releases it. Bootstrap
 * (absent->epoch1,checkpoint0) only when no owner/data exists. An existing
 * unowned v2 checkpoint may be adopted at epoch1 only when its exact current
 * token is supplied, replay-safe data validates and all old writers are quiescent.
 * v1 must first be explicitly saved as unowned v2; load alone never rewrites it.
 * All new-profile file save/append operations take this same session lock,
 * including unowned contexts; an unguarded context cannot modify owned data.
 * Identical retries
 * whose next+checkpoint are already committed revalidate/fsync before OK.
 * @note Source stops new work, drains/reaps, checkpoints, then releases its guard
 * and remains frozen BEFORE CAS. Target is inactive until it acquires next guard.
 * Aborting a prepared handoff resumes source only through a NEW epoch, including
 * same-instance return. Crash recovery may admit a fresh incarnation against
 * the exact latest generation only after old owned processes are fenced/reaped;
 * crashed STARTED/INDETERMINATE effect uncertainty then permits only
 * owner_acquire_recovery, never execution. Coherent PROPOSED remains eligible
 * for ordinary ownership and its explicit current policy/denial phase.
 * Uncertain IO never permits either side to guess or undo.
 */
cog_result_t cog_store_owner_compare_exchange(cog_store_t *store, const char *id,
                                               cog_store_owner_t expected,
                                               cog_store_owner_t next,
                                               cog_store_checkpoint_t checkpoint);
/**
 * @brief Explicit execution-guard constructor after durable owner admission.
 * @param[in,out] store Store requiring ownership. @param[in] id Session ID.
 * @param[in] expected Current admitted process-incarnation owner tuple.
 * @param[out] out_guard Owned guard, NULL on failure; match owner_release.
 * @return OK; AGAIN lock busy; INVALID_STATE stale owner; INDETERMINATE if
 * crashed STARTED/INDETERMINATE effect intent or uncertain manifest durability;
 * NOMEM/IO;
 * NULL_PTR; UNSUPPORTED absent independent-open-description locking/backend.
 * @post Reserve public wrapper/bookkeeping BEFORE invoking a custom normal or
 * recovery acquire callback. Reservation failure returns NOMEM, output NULL,
 * and invokes no backend acquire/release callback. Backend failure retains no
 * backend handle and leaves output NULL; release only the reserved wrapper.
 * After backend success, wrapping/linking/output publication cannot fail or
 * allocate, and does not call owner_release as a rollback mechanism.
 * @note Acquisition validates/fsync-confirms durable owner and current ledger
 * UNDER the lock. Coherent PROPOSED calls are proven unstarted and do not
 * require a recovery guard: ordinary ownership may resume their explicit policy
 * admission/denial on the same active path, while unrelated model/user/branch
 * work remains blocked. A loaded STARTED call instead becomes INDETERMINATE.
 * Owner_read alone never grants activation. Guard uses Linux
 * flock/OFD-style independently opened locks so two contexts in one process
 * also contend; no process-global lock table or process-scoped fcntl shortcut.
 * No lock descriptor inheritance into commands/aux cores: CLOEXEC and explicit
 * close in fork child. The OS releases it on owner death. Restart first fences/
 * reaps prior owned processes; unverified child effects remain INDETERMINATE,
 * never automatic failover/replay. A new incarnation requires a fresh epoch.
 * Unresolved intent is inspectable/reconcilable only through the explicitly
 * restricted owner_acquire_recovery path, never an automatic mode downgrade.
 */
cog_result_t cog_store_owner_acquire(cog_store_t *store, const char *id,
                                      cog_store_owner_t expected,
                                      cog_store_owner_guard_t **out_guard);
/**
 * @brief Acquire the exclusive lock for explicit recovery, never execution.
 * @param[in,out] store Fenced-profile context; one owner thread.
 * @param[in] id Session ID with a coherent v2 generation.
 * @param[in] expected Newly admitted current incarnation/epoch after old owned
 * process groups are fenced/reaped; never impersonate the dead incarnation.
 * @param[out] out_guard Owned recovery guard, NULL on failure; release normally.
 * @post Same pre-callback wrapper reservation and no-fallible-post-success
 * publication as owner_acquire; failure exposes no partial guard or ownership.
 * @return OK; AGAIN lock busy; INVALID_STATE stale tuple/no recovery needed;
 * NULL_PTR; NOMEM; IO; PROTOCOL corrupt authoritative data; UNSUPPORTED backend.
 * @note Takes the same independently opened lock and durably confirms owner as
 * ordinary acquire, but may hold unresolved ledger entries. It grants no model,
 * tool, child or input admission; guard_borrow fails INVALID_STATE. The only
 * permitted save changes are atomic reconcile_tool results/final states plus
 * their audit evidence; existing message bytes/node identity/parentage/opaque/
 * usage stay unchanged (only matching new result nodes may be appended), and
 * no new user/assistant/tool-call work may be added. save validates this monotonic
 * diff against the locked generation; conservative restored STARTED->
 * INDETERMINATE audit normalization is also permitted. Larger explicit
 * constructor reservations do not alter logical history and are allowed so a
 * valid recovery is not trapped by old max_nodes/payload limits. No in-place
 * growth; append/checkpoint_save refuse this guard.
 * After every unresolved entry is explicitly closed and save succeeds, release
 * recovery guard, CAS to a NEW epoch with the exact resulting checkpoint, then
 * ordinary acquire may grant execution. Failed save retains recovery ownership;
 * a crash repeats recovery without executing the call or forgetting uncertainty.
 */
cog_result_t cog_store_owner_acquire_recovery(cog_store_t *store, const char *id,
                                               cog_store_owner_t expected,
                                               cog_store_owner_guard_t **out_guard);

/**
 * @brief Borrow a live guard for one bounded execution constructor.
 * @param[in,out] guard Live owner guard. @param[in] session_id Matching ID.
 * @return OK; NULL_PTR; INVALID_STATE wrong session/inactive/recovery-only; LIMIT count overflow.
 * @note Owner thread, no allocation. Pair each success with guard_return after
 * execution is prepared/terminal and destroyed. Release refuses any borrowers.
 */
cog_result_t cog_store_owner_guard_borrow(cog_store_owner_guard_t *guard,
                                          const char *session_id);
/** @brief Return a constructor borrow after owned execution is drained/destroyed.
 * @param[in,out] guard Live guard. @return OK; NULL_PTR; INVALID_STATE unbalanced.
 * @note Owner thread; never releases the actual ownership lock. */
cog_result_t cog_store_owner_guard_return(cog_store_owner_guard_t *guard);
/** @brief Query whether a context requires fenced execution.
 * @param[in] store Context or NULL. @return true only when require_owner enabled.
 * @note Owner thread; no allocation or activation grant. */
bool cog_store_requires_owner(const cog_store_t *store);
/**
 * @brief Release/free one execution guard after quiescing every borrower.
 * @param[in,out] guard NULL is OK; live guard owned by caller.
 * @return OK consumes the live guard; INVALID_STATE is a nonconsuming refusal
 * while still borrowed or backend cannot accept release; IO or another backend
 * cleanup error consumes the guard while preserving the actual error.
 * @pre NonNULL guard is a live owned handle from the matching constructor.
 * No provider/tool/child execution remains; source stays application-frozen
 * after release until a new durable epoch and guard are acquired.
 * @post NULL is an OK no-op. For a valid nonNULL guard, every return except
 * INVALID_STATE ends BOTH public wrapper and backend-handle lifetimes. Caller
 * must clear its alias and never retry/borrow/return/free that consumed handle.
 * INVALID_STATE leaves the guard usable only under its existing authority and
 * lifetime: return outstanding borrows before another release attempt. No
 * cleanup resource is consumed on this refusal. Store outlives its live guards.
 * A consuming error is not successful lock-release or handoff evidence. Keep
 * the error and application frozen; do not advance CAS, thaw or replay work.
 * @note Owner thread; no allocation, implicit handoff, rollback, task retry or
 * peer wait. File and custom fenced backends obey the same ownership rule.
 */
cog_result_t cog_store_owner_release(cog_store_owner_guard_t *guard);

/* ------------------------------------------------------------------ */
/* Built-in backends                                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief File-based backend (default).
 *
 * Stores sessions as directories containing:
 *   - meta.json: session metadata (id, created_at, model, provider)
 *   - nodes.jsonl: append-only node log
 *   - index.bin: node_id → offset index for fast lookups
 *
 * v2 uses immutable generation files plus one atomically replaced manifest;
 * the names above describe the legacy v1 layout. Migration and commit contracts
 * above govern the v2 representation; unsupported compression fails explicitly.
 * This backend is always available.
 */
extern const cog_store_vtable_t cog_store_file_backend;

/**
 * @brief Null backend (no persistence).
 *
 * Useful for testing or when persistence is not needed.
 * Non-durable inspection/test backend. It cannot satisfy a durable STARTED
 * precondition; save/append return UNSUPPORTED instead of a false durable OK.
 * Loading nonexistent data returns NOT_FOUND; listing is empty.
 */
extern const cog_store_vtable_t cog_store_null_backend;

#ifdef __cplusplus
}
#endif

#endif /* COG_SESSION_STORE_H */
