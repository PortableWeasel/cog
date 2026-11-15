/**
 * @file config.h
 * @brief Hierarchical configuration tree with atomic commit and rollback.
 *
 * cog-config manages a tree of key-value nodes addressable by dotted
 * paths (e.g. "providers.openai.base-url").  Callers modify a candidate
 * copy within a transaction, then atomically commit or roll back.
 * Validators registered per subtree run before commit.
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
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p txn is NULL.
 * @return COG_ERROR_INVALID_ARG if a validator rejects the config.
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
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p txn, @p path, or @p out_value is NULL.
 * @return COG_ERROR_NOT_FOUND if the path does not exist.
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
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p txn or @p path is NULL.
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
 * @param[in]  path      Dotted path to list children of.
 * @param[out] out_keys  Caller-provided array of key pointers.
 * @param[in]  max_keys  Maximum number of keys to return.
 * @param[out] out_count Receives the actual number of children.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p txn, @p path, or @p out_count is NULL.
 * @return COG_ERROR_NOT_FOUND if the path does not exist.
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
 * Validators are called during cog_config_commit for every path that
 * matches the prefix.  Validators are stored in a static table;
 * there is no dynamic registration at runtime.
 *
 * @param[in,out] tree      Tree to register on.
 * @param[in]     prefix    Path prefix (e.g. "providers").
 * @param[in]     validator Validator function.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p tree, @p prefix, or @p validator is NULL.
 * @return COG_ERROR_OVERFLOW if the validator table is full.
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