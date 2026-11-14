/**
 * @file config_tree.h
 * @brief PoC 4: hierarchical configuration tree with commit/rollback.
 *
 * This is a proof-of-concept for the Configuration functional block of the
 * Cog design blueprint (docs/DESIGN.md). It implements a VyOS-style
 * hierarchical configuration model:
 *
 *   - a tree of named nodes addressable by dotted paths
 *     (e.g. "providers.openai.base-url");
 *   - a separate candidate tree that gathers staged changes;
 *   - validators registered per subtree, run against the candidate at
 *     commit time;
 *   - an atomic commit() that either installs the candidate as the new
 *     running tree or fails leaving the running tree untouched; and
 *   - a rollback() that restores the previous known-good running tree.
 *
 * Memory model:
 *   - `cog_cfg_t` is opaque: the caller allocates the storage itself
 *     (malloc / free) and passes it to cog_cfg_init(); cog_cfg_fini()
 *     frees every allocation made inside the object.
 *   - The library never allocates memory the caller cannot free with a
 *     matching function.
 *   - Node handles passed to validators are owned by the config object and
 *     are only valid for the duration of the commit call. Validators must
 *     not retain them.
 */

#ifndef COG_POC_CONFIG_TREE_H
#define COG_POC_CONFIG_TREE_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

/** Result codes returned by every API function. */
enum cog_cfg_status {
    COG_CFG_OK = 0,         /**< Success. */
    COG_CFG_EINVAL = -1,    /**< NULL argument, empty/malformed path, zero-length buffer. */
    COG_CFG_ENOMEM = -2,    /**< Allocation failed. */
    COG_CFG_ENOTFOUND = -3, /**< Path does not exist or holds no value. */
    COG_CFG_EVALIDATE = -4, /**< A registered validator rejected the candidate. */
    COG_CFG_ENOROLLBACK = -5, /**< No previous running tree to restore. */
    COG_CFG_EBUFSMALL = -6  /**< Value does not fit in the caller's buffer. */
};

/* ------------------------------------------------------------------ */
/* Opaque types                                                        */
/* ------------------------------------------------------------------ */

/** A configuration object: owns running + candidate + rollback trees. */
typedef struct cog_cfg cog_cfg_t;

/** A node in a configuration tree; opaque to callers. */
typedef struct cog_cfg_node cog_cfg_node_t;

/**
 * @brief Validator callback, registered against a subtree path.
 *
 * At commit time every registered validator is invoked once with the
 * candidate subtree at its registered path. The subtree handle is only
 * valid during the callback.
 *
 * @param[in] subtree  Candidate subtree rooted at the registered path.
 *                     NULL if the path does not exist in the candidate
 *                     (in that case the callback is not invoked at all).
 * @param[in] path     The path the validator was registered on.
 * @param[in] userdata User pointer passed to cog_cfg_register_validator().
 * @return 0 to accept the candidate, non-zero to reject it.
 */
typedef int (*cog_cfg_validator_fn)(const cog_cfg_node_t *subtree,
                                    const char *path,
                                    void *userdata);

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Size in bytes of the storage a caller must provide to house a
 * configuration object.
 *
 * The object's alignment is no stricter than malloc's guarantee
 * (verified by a compile-time assertion in the implementation), so
 * `malloc(cog_cfg_sizeof())` is a valid way to obtain storage:
 *
 *     cog_cfg_t *cfg = malloc(cog_cfg_sizeof());
 *     cog_cfg_init(cfg);
 *     ...
 *     cog_cfg_fini(cfg);
 *     free(cfg);
 *
 * @return Storage size in bytes (> 0).
 */
size_t cog_cfg_sizeof(void);

/**
 * @brief Initialise a configuration object.
 *
 * The caller provides the storage for @p cfg (see cog_cfg_sizeof()) and
 * owns it for the object's lifetime. The initial running tree is empty;
 * that empty configuration is remembered as the first known-good state,
 * so rollback() works even after the very first commit.
 *
 * @param[out] cfg Caller-provided storage to initialise.
 * @return COG_CFG_OK or COG_CFG_EINVAL / COG_CFG_ENOMEM.
 */
int cog_cfg_init(cog_cfg_t *cfg);

/**
 * @brief Tear down a configuration object, freeing every internal allocation.
 *
 * @param[in,out] cfg Object previously passed to cog_cfg_init().
 */
void cog_cfg_fini(cog_cfg_t *cfg);

/* ------------------------------------------------------------------ */
/* Candidate operations (never touch the running tree)                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Set a value at @p path in the candidate tree.
 *
 * The candidate is a session copy of the running tree plus staged edits
 * (VyOS-style), so setting works on committed keys too. Intermediate
 * nodes are created as needed. Any node may carry a value and children
 * at the same time. Setting replaces the previous value.
 *
 * @param[in,out] cfg   Configuration object.
 * @param[in]     path  Dotted path; must not be empty and must not
 *                      contain empty segments (e.g. "a..b", ".a", "a.").
 * @param[in]     value Value to store; may be "" but not NULL.
 * @return COG_CFG_OK or COG_CFG_EINVAL / COG_CFG_ENOMEM.
 */
int cog_cfg_set(cog_cfg_t *cfg, const char *path, const char *value);

/**
 * @brief Delete the subtree at @p path from the candidate tree.
 *
 * Works on nodes that were committed earlier, because the candidate is a
 * session copy of the running tree. Empty ancestors are pruned,
 * VyOS-style: deleting the last leaf under a subtree removes the
 * subtree. Deleting a container removes all of its descendants.
 *
 * @param[in,out] cfg  Configuration object.
 * @param[in]     path Dotted path of the node to remove.
 * @return COG_CFG_OK, COG_CFG_ENOTFOUND (path absent from candidate),
 *         or COG_CFG_EINVAL.
 */
int cog_cfg_delete(cog_cfg_t *cfg, const char *path);

/**
 * @brief Read a staged value from the candidate tree.
 *
 * Useful for review before committing, and for tests that assert the
 * candidate and running trees are independent.
 *
 * @param[in] cfg     Configuration object.
 * @param[in] path    Dotted path.
 * @param[out] buf    Buffer to receive the NUL-terminated value.
 * @param[in] bufsize Size of @p buf in bytes (>= 1).
 * @return COG_CFG_OK, COG_CFG_ENOTFOUND, COG_CFG_EINVAL, or COG_CFG_EBUFSMALL.
 */
int cog_cfg_candidate_get(const cog_cfg_t *cfg, const char *path,
                          char *buf, size_t bufsize);

/* ------------------------------------------------------------------ */
/* Running reads                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Read a value from the running tree.
 *
 * The running tree is what the rest of the system reads (per the design
 * blueprint). Staged candidate changes are not visible here until a
 * successful commit.
 *
 * @param[in] cfg     Configuration object.
 * @param[in] path    Dotted path.
 * @param[out] buf    Buffer to receive the NUL-terminated value.
 * @param[in] bufsize Size of @p buf in bytes (>= 1).
 * @return COG_CFG_OK, COG_CFG_ENOTFOUND (path missing or container
 *         without a value), COG_CFG_EINVAL, or COG_CFG_EBUFSMALL.
 */
int cog_cfg_get(const cog_cfg_t *cfg, const char *path,
                char *buf, size_t bufsize);

/* ------------------------------------------------------------------ */
/* Validators                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Register a validator for a subtree path.
 *
 * At commit time the validator runs only when its registered subtree is
 * *changed* in the candidate (marked dirty by set/delete since the last
 * commit or rollback); untouched subtrees are not re-validated. A
 * subtree deleted outright no longer exists and is skipped. An empty
 * path ("") registers a whole-tree validator that fires whenever
 * anything is staged.
 *
 * Validators run in registration order; the first rejection aborts the
 * commit.
 *
 * @param[in,out] cfg      Configuration object.
 * @param[in]     path     Dotted subtree path, or "" for the whole tree.
 * @param[in]     fn       Callback; must not be NULL.
 * @param[in]     userdata Opaque pointer passed through to @p fn.
 * @return COG_CFG_OK or COG_CFG_EINVAL / COG_CFG_ENOMEM.
 */
int cog_cfg_register_validator(cog_cfg_t *cfg, const char *path,
                               cog_cfg_validator_fn fn, void *userdata);

/* ------------------------------------------------------------------ */
/* Commit / rollback                                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief Atomically install the candidate tree as the running tree.
 *
 * Semantics:
 *  1. If nothing is staged (candidate clean), this is a successful no-op.
 *  2. Each registered validator runs against the candidate subtree it
 *     covers, but only when that subtree changed (is dirty).
 *  3. If any validator rejects the candidate, the staged changes are
 *     discarded, the running tree is left untouched (still the last
 *     known-good), and COG_CFG_EVALIDATE is returned.
 *  4. On success the validated candidate is promoted to be the running
 *     tree, the previous running tree becomes the rollback point, and a
 *     fresh clean candidate mirrors the new running tree.
 *
 * @param[in,out] cfg Configuration object.
 * @return COG_CFG_OK, COG_CFG_EVALIDATE, COG_CFG_ENOMEM, or COG_CFG_EINVAL.
 */
int cog_cfg_commit(cog_cfg_t *cfg);

/**
 * @brief Restore the running tree to the previous known-good state.
 *
 * This is a single-slot rollback: the tree that was running before the
 * most recent successful commit. Staged (uncommitted) candidate changes
 * are discarded, and the session candidate mirrors the restored tree. A
 * second consecutive rollback returns COG_CFG_ENOROLLBACK; a subsequent
 * commit re-arms the slot.
 *
 * @param[in,out] cfg Configuration object.
 * @return COG_CFG_OK, COG_CFG_ENOROLLBACK, or COG_CFG_EINVAL.
 */
int cog_cfg_rollback(cog_cfg_t *cfg);

/* ------------------------------------------------------------------ */
/* Display                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Print the running tree as one "path = value" line per node that
 * carries a value. Containers without values print nothing.
 *
 * @param[in] cfg Configuration object (may be NULL; prints nothing).
 * @param[in] out Output stream.
 */
void cog_cfg_show(const cog_cfg_t *cfg, FILE *out);

/**
 * @brief Print the candidate tree; same format as cog_cfg_show().
 *
 * @param[in] cfg Configuration object (may be NULL; prints nothing).
 * @param[in] out Output stream.
 */
void cog_cfg_show_candidate(const cog_cfg_t *cfg, FILE *out);

/* ------------------------------------------------------------------ */
/* Node inspection (for validators)                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Name of a node; "" for the tree root and for NULL.
 *
 * @param[in] node Node handle.
 * @return NUL-terminated name.
 */
const char *cog_cfg_node_name(const cog_cfg_node_t *node);

/**
 * @brief Value stored on a node, or NULL if it carries none.
 *
 * @param[in] node Node handle.
 * @return Value string or NULL.
 */
const char *cog_cfg_node_value(const cog_cfg_node_t *node);

/**
 * @brief Number of children of a node.
 *
 * @param[in] node Node handle.
 * @return Child count (0 for NULL).
 */
size_t cog_cfg_node_child_count(const cog_cfg_node_t *node);

/**
 * @brief Indexed access to a node's children.
 *
 * @param[in] node Node handle.
 * @param[in] idx  Index; must be < cog_cfg_node_child_count(node).
 * @return Child node or NULL if @p node is NULL or @p idx is out of range.
 */
const cog_cfg_node_t *cog_cfg_node_child(const cog_cfg_node_t *node, size_t idx);

/**
 * @brief Read a value from a subtree by path relative to @p root.
 *
 * Convenience for validators: e.g. reading "api-key" from a subtree
 * registered at "providers.openai".
 *
 * @param[in] root    Subtree root (typically the candidate subtree passed
 *                    to a validator).
 * @param[in] path    Dotted path relative to @p root.
 * @param[out] buf    Buffer to receive the NUL-terminated value.
 * @param[in] bufsize Size of @p buf in bytes (>= 1).
 * @return COG_CFG_OK, COG_CFG_ENOTFOUND, COG_CFG_EINVAL, or COG_CFG_EBUFSMALL.
 */
int cog_cfg_node_get_value(const cog_cfg_node_t *root, const char *path,
                           char *buf, size_t bufsize);

/* ------------------------------------------------------------------ */
/* Utilities                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Human-readable message for a result code.
 *
 * @param[in] code A cog_cfg_status value.
 * @return NUL-terminated string (never NULL).
 */
const char *cog_cfg_strerror(int code);

#ifdef __cplusplus
}
#endif

#endif /* COG_POC_CONFIG_TREE_H */
