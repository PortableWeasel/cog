/**
 * @file test_orchestrator_selfcontained.c
 * @brief Critic smoke test: verify cog/orchestrator.h is self-contained
 *        and the public types/enums are usable with strict warnings.
 */

#include "cog/orchestrator.h"

#include <stdio.h>

int
main(void)
{
    cog_orchestrator_config_t cfg = {0};
    cog_health_t h = COG_HEALTH_UNKNOWN;
    cog_core_info_t info = {0};
    cog_peer_id_t peer = COG_PEER_ID_INVALID;

    cfg.socket_path = "/tmp/cog-orch-smoke";
    cfg.max_aux_cores = 4U;
    cfg.health_interval_ms = 100U;
    cfg.unhealthy_threshold = 3U;
    cfg.default_core_capacity = 8U;
    cfg.auto_replace = true;

    info.peer_id = peer;
    info.pid = 0;
    info.health = h;
    info.active_subagents = 0U;
    info.capacity = 8U;
    info.last_heartbeat_ms = 0U;

    (void)cfg;
    (void)info;

    printf("smoke ok\n");
    return 0;
}
