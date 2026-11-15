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
 * Threading: every tree/transaction operation is owner-thread only, including
 * begin/get/list; no concurrent read/commit without external synchronization.
 * Copied immutable endpoint generations may be shared independently. All Cog
 * node/string/transaction/validator storage is constructor-reserved; no runtime
 * allocation/growth. Limit failure is atomic unless commit explicitly finalizes
 * a rejected transaction because its reset snapshot cannot fit.
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
 *
 * A finalized transaction's struct is retained by its tree until
 * cog_config_tree_free: stale-handle reuse is then detected
 * deterministically (COG_ERROR_INVALID_STATE for mutations,
 * COG_ERROR_NOT_FOUND for reads) and memory stays bounded by the
 * configured max_transactions handles, including finalized tombstones. Their
 * candidate storage is reclaimed on finalization; handle identity is never reused
 * within a tree. At the handle limit, begin returns LIMIT. A caller may construct
 * a new tree generation and retire this tree after its last borrower.
 */
typedef struct cog_config_transaction cog_config_txn_t;

/** @brief Aggregate constructor ceilings across running and all candidate trees. */
typedef struct cog_config_limits {
    size_t max_nodes; /**< Zero defaults 8192 aggregate nodes, not per transaction. */
    size_t string_bytes; /**< Zero defaults 8 MiB for paths/values/prefixes. */
    size_t max_transactions; /**< Zero defaults 4096 retained handle identities. */
    size_t max_validators; /**< Zero defaults 16 ordered validator slots. */
} cog_config_limits_t;

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
 * @brief Reserve bounded tree, candidate, tombstone and validator workspaces.
 * @param[out] out Owned empty tree; NULL on failure, match tree_free.
 * @param[in] limits NULL or zero fields select documented defaults. All size
 * arithmetic is checked; aggregate capacities must support at least one node
 * and transaction. No candidate/runtime operation can increase these limits.
 * @return OK; NULL_PTR; INVALID_ARG invalid/overflowing layout; NOMEM reservation.
 * @note Owner thread, no file/network I/O, validators or global state.
 */
cog_result_t cog_config_tree_new_ex(cog_config_tree_t **out,
                                    const cog_config_limits_t *limits);

/**
 * @brief Create a bounded tree, equivalent to tree_new_ex(out,NULL).
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
 * @return COG_ERROR_LIMIT if reserved node/string/transaction identity capacity
 * cannot fit the snapshot; failure sets *out_txn=NULL and changes no running data.
 * Threading: owner-thread only; no runtime allocation.
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
 * @return COG_ERROR_LIMIT if reserved workspace cannot re-derive the candidate
 *         after validator rejection (degraded path: the transaction is
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
 * @param[in]  path      Dotted path (see "Path grammar" above).
 * @param[out] out_value Receives a pointer to the value string, or NULL
 *                       on failure.  The pointer is borrowed from the
 *                       candidate tree: it is invalidated by any
 *                       subsequent set/delete on the same transaction
 *                       (they may free candidate storage) and by
 *                       finalization or tree destruction.  Callers must
 *                       not dereference it after any of those.
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
 * @return COG_ERROR_LIMIT if reserved node/string capacity cannot fit the
 * change; candidate/running data stay unchanged. No runtime allocation.
 * Threading: owner-thread only.
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
 *                       are borrowed from the candidate tree: they are
 *                       invalidated by any subsequent set/delete on the
 *                       same transaction (which may free node storage)
 *                       and by finalization or tree destruction.  Callers
 *                       must not dereference them after any of those.
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
 * (limits.max_validators, default 16): the table never grows at runtime, and
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
 * @return COG_ERROR_OVERFLOW if the configured validator table is full.
 * @return COG_ERROR_LIMIT if the reserved prefix-string pool is exhausted;
 * failure adds no validator and consumes no capacity. No runtime allocation.
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
