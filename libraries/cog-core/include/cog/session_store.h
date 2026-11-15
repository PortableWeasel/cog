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
/** @brief File load policy: unsafe legacy records require explicit inspection. */
typedef enum cog_store_load_mode {
    COG_STORE_LOAD_REPLAY = 0,
    COG_STORE_LOAD_INSPECT
} cog_store_load_mode_t;
/** @brief Facts returned by version-aware load; never stored credentials. */
typedef struct cog_store_load_report {
    uint32_t source_version;
    bool migrated_in_memory;
    bool recovered_truncated_tail;
    size_t discarded_tail_bytes;
    bool replay_safe;
    size_t indeterminate_tools;
} cog_store_load_report_t;

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
 * @return COG_OK on success.
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
 * Static const dispatch table — no dynamic registration.
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
     * committed prefix. Default false (IO on any truncation). A malformed
     * complete line, invalid association, bad checksum/version/index metadata
     * or middle corruption always fails IO/PROTOCOL; never skip records. */
    bool recover_truncated_tail;
    /** Constructor-reserved read/write/codec workspace; 0 selects 16 MiB.
     * Above capacity returns LIMIT, never hidden growth or partial publication. */
    size_t max_record_bytes;
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
 * No credential/raw authorization bytes may be persisted.
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
 * @note Recovery is enabled only by recover_truncated_tail; no disk writes.
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
 * @return COG_OK on success.
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
 * All operations return COG_OK but do nothing.
 */
extern const cog_store_vtable_t cog_store_null_backend;

#ifdef __cplusplus
}
#endif

#endif /* COG_SESSION_STORE_H */
