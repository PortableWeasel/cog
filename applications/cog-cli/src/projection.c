/** @file projection.c @brief Atomic copied canonical view and replay owner. */
#include "view_internal.h"
#include "codec_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static bool add_size(size_t a, size_t b, size_t *out)
{
    if (b > SIZE_MAX - a) return false;
    *out = a + b;
    return true;
}

void cog_cli_projection_free(cog_cli_projection_t *projection)
{
    if (projection == NULL) return;
    cog_ipc_replay_free(projection->replay);
    for (size_t i = 0U; i < 2U; ++i) {
        free(projection->banks[i].items);
        free(projection->banks[i].text);
        free(projection->banks[i].wire);
    }
    free(projection);
}

cog_result_t cog_cli_projection_new(cog_cli_projection_t **out,
    const cog_cli_projection_config_t *config)
{
    if (out == NULL) return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (config == NULL) return COG_ERROR_NULL_PTR;
    if (config->session_id == NULL || config->generation == 0U ||
        config->records == 0U || config->replay_bytes == 0U ||
        config->projection_bytes == 0U || config->max_display_bytes == 0U ||
        config->max_agents > CLI_VIEW_AGENTS || config->max_status_bytes == 0U ||
        config->max_result_bytes == 0U) return COG_ERROR_INVALID_ARG;
    size_t sid = strlen(config->session_id);
    if (sid == 0U || sid > 127U || !cli_wire_session(config->session_id, sid))
        return COG_ERROR_INVALID_ARG;
    size_t row = 174U, worst = 452U;
    if (!add_size(row, config->max_status_bytes, &row) ||
        !add_size(row, config->max_result_bytes, &row) ||
        (config->max_agents != 0U && row > SIZE_MAX / config->max_agents) ||
        !add_size(worst, row * config->max_agents, &worst) ||
        !add_size(worst, config->max_display_bytes, &worst) ||
        worst > config->projection_bytes || worst > config->replay_bytes ||
        worst > UINT32_MAX || config->max_display_bytes / 14U >
            SIZE_MAX / sizeof(cog_cli_display_item_t)) return COG_ERROR_LIMIT;
    cog_cli_projection_t *p = calloc(1U, sizeof(*p));
    if (p == NULL) return COG_ERROR_NOMEM;
    p->fixed = *config;
    memcpy(p->session, config->session_id, sid + 1U);
    p->fixed.session_id = p->session;
    p->item_capacity = config->max_display_bytes / 14U;
    cog_result_t rc = cog_ipc_replay_new(&p->replay, p->session,
        config->generation, config->records, config->replay_bytes);
    if (rc != COG_OK) { cog_cli_projection_free(p); return rc; }
    for (size_t i = 0U; i < 2U; ++i) {
        cli_view_bank_t *b = &p->banks[i];
        b->items = calloc(p->item_capacity == 0U ? 1U : p->item_capacity, sizeof(*b->items));
        b->text = malloc(config->projection_bytes);
        b->wire = malloc(config->projection_bytes);
        if (b->items == NULL || b->text == NULL || b->wire == NULL) {
            cog_cli_projection_free(p); return COG_ERROR_NOMEM;
        }
    }
    *out = p;
    return COG_OK;
}

cog_result_t cog_cli_projection_commit(cog_cli_projection_t *p,
    const cog_cli_projection_view_t *source, uint64_t *sequence)
{
    if (p == NULL || source == NULL || sequence == NULL) return COG_ERROR_NULL_PTR;
    if (p->sequence == UINT64_MAX) return COG_ERROR_LIMIT;
    if (source->display.session_id == NULL ||
        strcmp(source->display.session_id, p->session) != 0 ||
        source->agent_count > p->fixed.max_agents ||
        (source->agent_count != 0U && source->agents == NULL)) return COG_ERROR_INVALID_ARG;
    for (size_t i = 0U; i < source->agent_count; ++i) {
        const cog_cli_agent_view_t *a = &source->agents[i];
        if (a->status_text == NULL || a->result_text == NULL) return COG_ERROR_INVALID_ARG;
        if (strlen(a->status_text) >= p->fixed.max_status_bytes ||
            strlen(a->result_text) >= p->fixed.max_result_bytes) return COG_ERROR_LIMIT;
    }
    cog_cli_projection_view_t staged = *source;
    staged.display.session_id = p->session;
    staged.display.generation = p->fixed.generation;
    staged.display.through_sequence = p->sequence + 1U;
    size_t size = 0U;
    cog_result_t rc = cog_cli_display_snapshot_encode(&staged.display, NULL, 0U, &size);
    if (rc != COG_ERROR_OVERFLOW && rc != COG_OK) return rc;
    if (size > p->fixed.max_display_bytes) return COG_ERROR_LIMIT;
    cli_view_bank_t *bank = &p->banks[1U - p->current];
    rc = cog_cli_projection_encode(&staged, bank->wire, p->fixed.projection_bytes, &size);
    if (rc != COG_OK) return rc == COG_ERROR_OVERFLOW ? COG_ERROR_LIMIT : rc;
    size_t items = 0U, agents = 0U, policies = 0U, text = 0U;
    rc = cog_cli_projection_decode(bank->wire, size, &bank->view, bank->items,
        p->item_capacity, bank->agents, CLI_VIEW_AGENTS, bank->policies,
        CLI_VIEW_POLICIES, bank->text, p->fixed.projection_bytes,
        &items, &agents, &policies, &text);
    if (rc != COG_OK) return rc == COG_ERROR_OVERFLOW ? COG_ERROR_LIMIT : rc;
    uint64_t assigned = 0U;
    rc = cog_ipc_replay_append(p->replay, CLI_VIEW_REPLAY_TYPE, bank->wire, size, &assigned);
    if (rc != COG_OK) return rc;
    assert(assigned == staged.display.through_sequence);
    bank->wire_length = size;
    p->current = 1U - p->current;
    p->sequence = assigned;
    p->committed = true;
    *sequence = assigned;
    return COG_OK;
}

cog_result_t cog_cli_projection_snapshot(const cog_cli_projection_t *p,
    cog_cli_projection_view_t *view)
{
    if (p == NULL || view == NULL) return COG_ERROR_NULL_PTR;
    if (!p->committed) return COG_ERROR_AGAIN;
    *view = p->banks[p->current].view;
    return COG_OK;
}

cog_result_t cog_cli_projection_copy_after(const cog_cli_projection_t *p,
    uint64_t generation, uint64_t after_sequence, cog_ipc_replay_record_t *record,
    void *storage, size_t capacity, size_t *required)
{
    if (p == NULL) return COG_ERROR_NULL_PTR;
    return cog_ipc_replay_copy_after(p->replay, generation, after_sequence,
        record, storage, capacity, required);
}
