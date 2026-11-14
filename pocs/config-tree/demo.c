/**
 * @file demo.c
 * @brief Walk-through demo for the config-tree PoC.
 *
 * Exercises the acceptance criteria end to end:
 *   - set() on dotted paths (providers.openai.base-url style)
 *   - candidate changes invisible to running readers
 *   - per-subtree validators run at commit time
 *   - atomic commit: rejection leaves the running tree untouched
 *   - rollback() restores the previous known-good running tree
 *
 * Memory pattern: `cog_cfg_t` is opaque, so the caller allocates the
 * storage (malloc) and passes it to cog_cfg_init(); cog_cfg_fini() frees
 * every internal allocation and the caller frees the struct.
 */

#include <stdio.h>
#include <stdlib.h>

#include "config_tree.h"

static int demo_validate_openai(const cog_cfg_node_t *subtree,
                                const char *path, void *userdata)
{
    (void)userdata;
    char buf[256];
    if (cog_cfg_node_get_value(subtree, "api-key", buf, sizeof buf) != COG_CFG_OK) {
        fprintf(stderr, "  validator <%s>: missing required child 'api-key'\n",
                path);
        return -1;
    }
    if (buf[0] == '\0') {
        fprintf(stderr, "  validator <%s>: api-key must not be empty\n", path);
        return -1;
    }
    return 0;
}

static int demo_validate_has_provider(const cog_cfg_node_t *subtree,
                                      const char *path, void *userdata)
{
    (void)subtree;
    (void)path;
    (void)userdata;
    /* Whole-tree validator: refuse a commit that would leave no provider. */
    return cog_cfg_node_child_count(subtree) == 0U ? -1 : 0;
}

static void demo_show_running(cog_cfg_t *cfg)
{
    printf("  running:\n");
    cog_cfg_show(cfg, stdout);
}

static void demo_show_candidate(cog_cfg_t *cfg)
{
    printf("  candidate:\n");
    cog_cfg_show_candidate(cfg, stdout);
}

int main(void)
{
    cog_cfg_t *cfg = (cog_cfg_t *)malloc(cog_cfg_sizeof());
    char buf[256];

    printf("== PoC 4: hierarchical config commit/rollback ==\n\n");

    if (cfg == NULL || cog_cfg_init(cfg) != COG_CFG_OK) {
        fprintf(stderr, "init failed\n");
        free(cfg);
        return 1;
    }

    /* Validators are registered per subtree. */
    printf("[1] Register per-subtree validators:\n");
    printf("    - providers.openai: api-key must be present and non-empty\n");
    printf("    - <root>: running config must keep at least one provider\n");
    (void)cog_cfg_register_validator(cfg, "providers.openai",
                                     demo_validate_openai, NULL);
    (void)cog_cfg_register_validator(cfg, "", demo_validate_has_provider, NULL);

    /* Stage a candidate without touching the running tree. */
    printf("\n[2] Stage provider config in the candidate tree:\n");
    (void)cog_cfg_set(cfg, "providers.openai.base-url",
                      "https://api.openai.com/v1");
    (void)cog_cfg_set(cfg, "providers.openai.api-key", "sk-openai-demo");
    (void)cog_cfg_set(cfg, "providers.openai.model", "gpt-4o");
    (void)cog_cfg_set(cfg, "providers.anthropic.base-url",
                      "https://api.anthropic.com/v1");
    (void)cog_cfg_set(cfg, "providers.anthropic.model", "claude-4");
    demo_show_candidate(cfg);
    demo_show_running(cfg);

    printf("\n  candidates are invisible to readers: get(base-url) = %s\n",
           cog_cfg_strerror(cog_cfg_get(cfg, "providers.openai.base-url",
                                        buf, sizeof buf)));

    /* Commit: validators pass, running tree is installed. */
    printf("\n[3] Commit candidate (validators pass):\n");
    if (cog_cfg_commit(cfg) != COG_CFG_OK) {
        fprintf(stderr, "  unexpected commit failure\n");
        cog_cfg_fini(cfg);
        free(cfg);
        return 1;
    }
    demo_show_running(cfg);

    printf("\n[4] Staged change is now visible to readers:\n");
    (void)cog_cfg_get(cfg, "providers.openai.base-url", buf, sizeof buf);
    printf("    providers.openai.base-url = %s\n", buf);

    /* A bad candidate is rejected atomically. */
    printf("\n[5] Stage a change that violates the openai validator:\n");
    (void)cog_cfg_set(cfg, "providers.openai.api-key", "");
    printf("    commit -> %s\n",
           cog_cfg_strerror(cog_cfg_commit(cfg)));
    printf("    running tree untouched (api-key still present):\n");
    demo_show_running(cfg);

    /* Fix the candidate and commit again. */
    printf("\n[6] Fix the candidate and commit:\n");
    (void)cog_cfg_set(cfg, "providers.openai.api-key", "sk-revised");
    (void)cog_cfg_set(cfg, "providers.openai.base-url",
                      "https://api.openai.com/v1/revised");
    if (cog_cfg_commit(cfg) != COG_CFG_OK) {
        fprintf(stderr, "  unexpected commit failure\n");
        cog_cfg_fini(cfg);
        free(cfg);
        return 1;
    }
    demo_show_running(cfg);

    /* Rollback restores the previous known-good tree. */
    printf("\n[7] Rollback to the previous known-good running tree:\n");
    if (cog_cfg_rollback(cfg) != COG_CFG_OK) {
        fprintf(stderr, "  unexpected rollback failure\n");
        cog_cfg_fini(cfg);
        free(cfg);
        return 1;
    }
    demo_show_running(cfg);
    (void)cog_cfg_get(cfg, "providers.openai.api-key", buf, sizeof buf);
    printf("    providers.openai.api-key = %s (original value restored)\n", buf);

    /* Rollback again: single-slot semantics mean the known-good point
     * from before commit A is already consumed — expect ENOROLLBACK and
     * an unchanged running tree. */
    printf("\n[8] Rollback again (single-slot semantics):\n");
    printf("    rollback -> %s\n", cog_cfg_strerror(cog_cfg_rollback(cfg)));
    demo_show_running(cfg);
    printf("    running configuration is unchanged after the failed rollback\n");

    cog_cfg_fini(cfg);
    free(cfg);
    printf("\nDemo complete.\n");
    return 0;
}
