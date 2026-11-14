/**
 * @file test_core_header.c
 * @brief Critic smoke test: verify cog/core.h is self-contained and
 *        the public types/enums are usable with strict warnings.
 */

#include "cog/core.h"

#include <stdio.h>

static cog_handler_result_t dummy_handler(cog_core_t *core,
                                           const cog_event_t *event)
{
    (void)core;
    (void)event;
    return (cog_handler_result_t){COG_TRANSITION_SELF, COG_OK};
}

static cog_result_t dummy_tool(cog_core_t *core,
                                const char *request,
                                size_t request_len,
                                char *result,
                                size_t result_capacity,
                                size_t *out_result_len)
{
    (void)core;
    (void)request;
    (void)request_len;
    (void)result;
    (void)result_capacity;
    (void)out_result_len;
    return COG_ERROR_NOT_FOUND;
}

int main(void)
{
    static const cog_state_t allowed[] = {
        COG_STATE_MAX,
        COG_TRANSITION_SELF,
        COG_TRANSITION_END,
    };

    static const cog_dispatch_entry_t table[] = {
        COG_DISPATCH_ENTRY(0U, 1U, dummy_handler, allowed),
    };

    static const cog_tool_t tools[] = {
        {"dummy", "A dummy tool.", NULL, dummy_tool},
    };

    const cog_tool_table_t tool_table = {tools, 1U};

    cog_event_t event = {
        .id = 1U,
        .kind = COG_EVENT_USER_INPUT,
        .payload = NULL,
        .payload_len = 0U,
        .source_id = 0U,
    };

    (void)table;
    (void)tool_table;
    (void)event;

    printf("smoke ok\n");
    return 0;
}
