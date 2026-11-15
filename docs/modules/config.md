# cog-config {#page_cog_config}

## What it does

`cog-config` is a hierarchical configuration tree addressed by dotted paths
(`providers.openai.base-url`). It gives you:

- a **running configuration** that readers see,
- **candidate transactions** staged against it,
- **atomic commit** or **rollback**,
- **validators** that run at commit time,
- **bounded resources** reserved at construction.

This is what makes hot reload safe: a reload builds a complete candidate and
publishes it in one step, or leaves the running configuration untouched.

Headers: `config.h`.

## Key ideas

**Dotted paths, string values.** Every leaf holds one `const char *`. Path
segments form containers implicitly; there is no separate schema type.

**Transactions isolate readers.** `cog_config_begin` creates a candidate.
Readers of the running tree keep seeing the old values until
`cog_config_commit` swaps the candidate in. Commit is atomic.

**Borrowed values have a short life.** A pointer returned by
`cog_config_get` or `cog_config_list` stays valid only until the next
`set`/`delete` on the same transaction, or until the transaction is
finalized. Copy what you need to keep.

**Everything is bounded.** `cog_config_tree_new_ex` reserves node, string,
transaction-handle, and validator capacity up front. Exhausting a limit
returns `COG_ERROR_LIMIT` and leaves the candidate unchanged.

**Commit can be rejected.** Validators run in registration order during
commit. A rejection fails the commit, keeps the running configuration
untouched, and leaves the transaction usable so the caller can fix and retry.

## Getting started: stage, commit, read

```c
#include <cog/config.h>

#include <stdio.h>

int main(void)
{
    cog_config_tree_t *tree = NULL;
    cog_config_txn_t *txn = NULL;
    const char *base_url = NULL;

    if (cog_config_tree_new(&tree) != COG_OK) {
        return 1;
    }

    /* 1. Stage a candidate configuration. */
    if (cog_config_begin(tree, &txn) != COG_OK) {
        cog_config_tree_free(tree);
        return 1;
    }

    if (cog_config_set(txn, "providers.openai.base-url",
                       "https://api.openai.com") != COG_OK ||
        cog_config_set(txn, "providers.openai.api-key", "sk-example") != COG_OK ||
        cog_config_set(txn, "models.default", "gpt-4o-mini") != COG_OK) {
        cog_config_rollback(txn);
        cog_config_tree_free(tree);
        return 1;
    }

    if (cog_config_commit(txn) != COG_OK) {
        /* Commit failed: the running config is unchanged. */
        cog_config_tree_free(tree);
        return 1;
    }

    /* 2. Read the running configuration through a fresh transaction. */
    if (cog_config_begin(tree, &txn) != COG_OK) {
        cog_config_tree_free(tree);
        return 1;
    }

    if (cog_config_get(txn, "providers.openai.base-url", &base_url) == COG_OK) {
        printf("base url: %s\n", base_url);
    }

    cog_config_rollback(txn); /* read-only: nothing to publish */
    cog_config_tree_free(tree);
    return 0;
}
```

## Validators

A validator is a function that inspects the candidate for a path prefix and
returns `COG_OK` or an error. Registering one is how a reload refuses to
publish an inconsistent configuration.

```c
#include <cog/config.h>

#include <stdio.h>

/* Reject a candidate whose active provider has no base URL. */
static cog_result_t require_base_url(const cog_config_txn_t *txn,
                                     const char *prefix)
{
    char path[128];
    const char *value = NULL;

    (void)snprintf(path, sizeof path, "%s.base-url", prefix);
    if (cog_config_get(txn, path, &value) != COG_OK ||
        value == NULL || value[0] == '\0') {
        return COG_ERROR_INVALID_ARG;
    }
    return COG_OK;
}

cog_result_t install_validator(cog_config_tree_t *tree);

cog_result_t install_validator(cog_config_tree_t *tree)
{
    return cog_config_register_validator(tree, "providers.openai",
                                         require_base_url);
}
```

The validator signature has no user-data parameter, so validators must be
file-static functions or otherwise free of per-instance state. That is a
deliberate constraint: the config tree stays a pure value, not a bag of
callbacks with lifetimes.

## Beyond the basics

- **Listing.** `cog_config_list(txn, prefix, ...)` enumerates the immediate
  children of a container, with the same borrowed-pointer rules as `get`.
- **Deletion and pruning.** `cog_config_delete` removes a subtree; empty,
  valueless ancestors are pruned automatically.
- **Hot reload.** The usual flow is: read the new file into a fresh tree,
  copy values into a candidate transaction, run validators, commit, then
  publish the new generation to components that cache configuration.
- **Limits.** Size the tree for the largest configuration you will ever
  accept. There is no growth after construction.

## See also

- @ref page_cog_provider — the values this module typically configures
- `pocs/config-tree` — the original proof of concept
- `applications/cog-cli/CONFIGURATION.md` — the CLI's key set
