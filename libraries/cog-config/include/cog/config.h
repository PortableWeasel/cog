/**
 * @file config.h
 * @brief Hierarchical configuration tree with atomic commit and rollback.
 *
 * cog-config manages a tree of key-value nodes addressable by dotted
 * paths (e.g. "providers.openai.base-url").  Callers modify a candidate
 * copy within a transaction, then atomically commit or roll back.
 * Validators registered per subtree run before commit.
 *
 * Reading is transactional: values are read from a transaction's
 * candidate tree, so a snapshot taken before a commit is unaffected by
 * that commit (hot-reload isolation).  To read the running
 * configuration, begin a transaction, read from it, and roll it back.
 *
 * Path grammar: a path is one or more dot-separated segments, each
 * segment non-empty (e.g. "providers.openai.base-url").  An empty path
 * "" and paths with empty segments (".", "a..b", ".a", "a.") are
 * rejected with COG_ERROR_INVALID_ARG, except that cog_config_list
 * accepts "" for the tree root (listing top-level keys).  A node may
 * hold both a value and children.  "Missing key" is reported as
 * COG_ERROR_NOT_FOUND, as is a path that resolves to a node carrying no
 * value.
 *
 * Threading model: a cog_config_tree_t is not thread-safe.  Transactions
 * are single-threaded.  The running tree may be read concurrently after
 * commit; the candidate tree is private to the transaction.
 */
#ifndef COG_CONFIG_H
#define COG_CONFIG_H

#include <cog/result.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque configuration tree.
 *
 * Created with cog_config_tree_new and destroyed with
 * cog_config_tree_free.  Holds the running (committed) configuration.
 */
typedef struct cog_config_tree cog_config_tree_t;

/**
 * @brief Opaque configuration transaction.
 *
 * Created by cog_config_begin and finalized by cog_config_commit or
 * cog_config_rollback.  Holds the candidate (uncommitted) configuration.
 */
typedef struct cog_config_transaction cog_config_txn_t;

/**
 * @brief Validator function for a config subtree.
 *
 * Called during cog_config_commit for each registered path prefix.
 * The validator receives the candidate tree and the path prefix.
 *
 * @param[in] txn    Transaction containing the candidate config.
 * @param[in] prefix Path prefix this validator is responsible for.
 * @return COG_OK if the subtree is valid.
 * @return COG_ERROR_INVALID_ARG if validation fails (with details
 *         logged via cog-obs).
 */
typedef cog_result_t (*cog_config_validator_fn)(const cog_config_txn_t *txn,
                                                 const char *prefix);

/* ── Lifecycle ──────────────────────────────────────────────────── */

/**
 * @brief Create a new configuration tree.
 *
 * @param[out] out Uninitialised pointer; set to the new tree on success.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p out is NULL.
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * @post On success, @p *out is a valid empty tree.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_config_tree_new(cog_config_tree_t **out);

/**
 * @brief Destroy a configuration tree.
 *
 * @param[in,out] tree Tree to destroy; may be NULL (no-op).
 *
 * Threading: not thread-safe.
 */
void cog_config_tree_free(cog_config_tree_t *tree);

/* ── Transactions ───────────────────────────────────────────────── */

/**
 * @brief Begin a configuration transaction.
 *
 * Creates a candidate copy of the running tree.  Modifications are
 * made to the candidate; the running tree is unaffected until commit.
 *
 * @param[in]  tree    Tree to transact on.
 * @param[out] out_txn Receives the transaction handle.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p tree or @p out_txn is NULL.
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_config_begin(cog_config_tree_t *tree,
                               cog_config_txn_t **out_txn);

/**
 * @brief Atomically commit a transaction.
 *
 * Runs all registered validators.  If any validator fails, the
 * candidate is discarded and the running tree is unchanged.
 * On success, the candidate atomically replaces the running tree.
 *
 * @param[in,out] txn Transaction to commit.
 * Validators run in registration order, each fire only when its prefix
 * exists in the candidate (a prefix removed by delete, or never
 * present, is skipped; the empty prefix "" is a whole-tree validator
 * that fires on every commit).  If a validator rejects the candidate,
 * the staged edits are discarded (the candidate is reset to a clean
 * copy of the running tree), the running tree is unchanged, and the
 * transaction remains usable for a further attempt.
 *
 * On success the candidate atomically replaces the running tree and the
 * transaction is finalized: commit/rollback/set/delete afterwards return
 * COG_ERROR_INVALID_STATE and get/list return COG_ERROR_NOT_FOUND.
 *
 * @param[in,out] txn Transaction to commit.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p txn is NULL.
 * @return COG_ERROR_INVALID_ARG if a validator rejects the config.
 * @return COG_ERROR_NOMEM if the candidate cannot be re-derived after a
 *         validator rejection (degraded path: the transaction is
 *         finalized in that case).
 * @return COG_ERROR_INVALID_STATE if the transaction was already
 *         committed or rolled back.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_config_commit(cog_config_txn_t *txn);

/**
 * @brief Discard a transaction without committing.
 *
 * The candidate is discarded; the running tree is unchanged.
 *
 * @param[in,out] txn Transaction to discard; may be NULL (no-op).
 *                    Discarding an already-finalized transaction is
 *                    also a no-op.
 *
 * Threading: not thread-safe.
 */
void cog_config_rollback(cog_config_txn_t *txn);

/* ── Path-based access ──────────────────────────────────────────── */

/**
 * @brief Get a value by dotted path.
 *
 * @param[in]  txn       Transaction to read from.
 * @param[in]  path      Dotted path (e.g. "providers.openai.base-url").
 * @param[out] out_value Receives the value string, or NULL if not found.
 * @param[in]  txn       Transaction to read from.
 * @param[in]  path      Dotted path (see "Path grammar" above).
 * @param[out] out_value Receives a pointer to the value string, or NULL
 *                       on failure.  The pointer is owned by the
 *                       transaction's candidate tree and stays valid
 *                       until the transaction is finalized.
 * @return COG_OK on success; @p *out_value points into the candidate.
 * @return COG_ERROR_NULL_PTR if @p txn, @p path, or @p out_value is NULL.
 * @return COG_ERROR_INVALID_ARG if @p path is empty or malformed.
 * @return COG_ERROR_NOT_FOUND if the path does not exist, resolves to a
 *         node that carries no value, or the transaction was already
 *         finalized.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_config_get(const cog_config_txn_t *txn,
                             const char *path,
                             const char **out_value);

/**
 * @brief Set a value by dotted path.
 *
 * Creates intermediate nodes as needed.  The value string is copied.
 *
 * @param[in,out] txn   Transaction to modify.
 * @param[in]     path  Dotted path.
 * @param[in]     value Value to set; copied.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p txn, @p path, or @p value is NULL.
 * @return COG_ERROR_INVALID_ARG if @p path is empty or malformed.
 * @return COG_ERROR_INVALID_STATE if @p txn was already finalized.
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_config_set(cog_config_txn_t *txn,
                             const char *path,
                             const char *value);

/**
 * @brief Delete a subtree by dotted path.
 *
 * @param[in,out] txn  Transaction to modify.
 * @param[in]     path Dotted path to delete.
 * Deleting the last child of a valueless subtree removes the subtree
 * (empty, valueless ancestors are pruned).  Deleting a container
 * removes all of its descendants.
 *
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p txn or @p path is NULL.
 * @return COG_ERROR_INVALID_ARG if @p path is empty or malformed.
 * @return COG_ERROR_INVALID_STATE if @p txn was already finalized.
 * @return COG_ERROR_NOT_FOUND if the path does not exist.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_config_delete(cog_config_txn_t *txn,
                                const char *path);

/**
 * @brief List children at a path.
 *
 * @param[in]  txn       Transaction to read from.
 * @param[in]  path      Dotted path to list children of; "" lists the
 *                       tree root's children.
 * @param[out] out_keys  Caller-provided array of key pointers (may be
 *                       NULL when @p max_keys is zero).  The pointers
 *                       are owned by the candidate tree and stay valid
 *                       until the transaction is finalized.
 * @param[in]  max_keys  Maximum number of keys to return; zero means
 *                       count-only (no keys are written).
 * @param[out] out_count Receives the actual number of children.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p txn, @p path, or @p out_count is NULL,
 *         or if @p max_keys > 0 and @p out_keys is NULL.
 * @return COG_ERROR_INVALID_ARG if @p path is malformed or non-empty
 *         with empty segments ("" is valid).
 * @return COG_ERROR_NOT_FOUND if the path does not exist or the
 *         transaction was already finalized.
 * @return COG_ERROR_OVERFLOW if the node has more than @p max_keys
 *         children (and @p max_keys > 0); @p *out_count is set to the
 *         real count.  No keys are truncated.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_config_list(const cog_config_txn_t *txn,
                              const char *path,
                              const char **out_keys,
                              size_t max_keys,
                              size_t *out_count);

/* ── Validators ─────────────────────────────────────────────────── */

/**
 * @brief Register a validator for a path prefix.
 *
 * Validators are called during cog_config_commit for every registered
 * prefix that exists in the candidate (see cog_config_commit).  They
 * run in registration order; the first rejection aborts the commit.
 * Validators are stored in a fixed-capacity table embedded in the tree
 * (16 slots per tree): the table never grows at runtime, and
 * COG_ERROR_OVERFLOW reports a full table.
 *
 * @param[in,out] tree      Tree to register on.
 * @param[in]     prefix    Path prefix (e.g. "providers"); "" registers
 *                          a whole-tree validator that fires on every
 *                          commit.  Duplicate prefixes are allowed.
 * @param[in]     validator Validator function.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p tree, @p prefix, or @p validator is NULL.
 * @return COG_ERROR_INVALID_ARG if @p prefix is a malformed path.
 * @return COG_ERROR_OVERFLOW if the validator table is full (16 entries).
 * @return COG_ERROR_NOMEM if the prefix cannot be copied.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_config_register_validator(cog_config_tree_t *tree,
                                            const char *prefix,
                                            cog_config_validator_fn validator);

#ifdef __cplusplus
}
#endif

#endif /* COG_CONFIG_H */
