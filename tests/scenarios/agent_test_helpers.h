#ifndef COG_SCENARIO_AGENT_TEST_HELPERS_H
#define COG_SCENARIO_AGENT_TEST_HELPERS_H

#include <cog/agent.h>

/* Test driver copies and explicitly acknowledges the last snapshot before
 * submitting the next correlated local attempt update. */
static cog_result_t scenario_agent_step(cog_core_t *core, cog_agent_id_t id,
                                        cog_agent_status_t status, cog_result_t outcome,
                                        const char *data, size_t length)
{
    cog_agent_update_t previous, update = {0U, id, 1U, status, outcome, data, length};
    char storage[65536];
    size_t needed;
    cog_result_t rc = cog_agent_copy_update(core, id, &previous, storage, sizeof(storage), &needed);
    if (rc == COG_OK) {
        rc = cog_agent_ack_update(core, id, previous.execution_epoch, previous.sequence);
        if (rc != COG_OK) return rc;
        update.sequence = previous.sequence + 1U;
    } else if (rc != COG_ERROR_AGAIN) {
        return rc;
    }
    return cog_agent_apply_update(core, id, &update);
}

#endif
