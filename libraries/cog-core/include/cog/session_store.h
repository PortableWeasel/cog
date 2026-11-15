/**
 * @file session_store.h
 * @brief Pluggable session persistence backend.
 *
 * Defines the interface for long-term storage of conversation sessions.
 * Sessions are trees of nodes with messages, timing, billing, and provider
 * data. The store is responsible for persisting and loading sessions.
 *
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
     * Zero means unlimited. When exceeded, oldest sessions are pruned.
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
 * @brief Save a session.
 *
 * @param[in] store   Store instance.
 * @param[in] session Session to save.
 * @return COG_OK on success.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_store_save(cog_store_t *store,
                             const cog_session_t *session);

/**
 * @brief Load a session.
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
 * @brief Append a single node to a persisted session.
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
