/**
 * @file test_agent_selfcontained.c
 * @brief Critic smoke test: verify cog/agent.h is self-contained and
 *        the public types/enums are usable with strict warnings.
 */

#include "cog/agent.h"

#include <stdio.h>

int main(void)
{
    cog_agent_config_t cfg = {0};
    cog_agent_status_t st = COG_AGENT_STATUS_PENDING;
    cog_agent_id_t id = 0U;

    cfg.task = "smoke";
    cfg.parent_session_id = "parent";
    cfg.timeout_ms = 100U;
    cfg.force_local = true;
    cfg.group_id = 1U;

    (void)cfg;
    (void)st;
    (void)id;

    printf("smoke ok\n");
    return 0;
}
