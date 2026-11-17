/** @file projection_codec.c @brief Transactional full CGV1 replacement codec. */
#include "projection.h"
#include "codec_internal.h"

static bool agent_valid(const cog_cli_agent_view_t *a, size_t sid, size_t result)
{
    if (a->agent_id == 0U || a->execution_epoch == 0U || a->group_id == 0U ||
        a->configuration_generation == 0U || !cli_wire_session(a->session_id, sid) ||
        a->status < COG_AGENT_STATUS_PENDING || a->status > COG_AGENT_STATUS_TIMEOUT) return false;
    if (a->status <= COG_AGENT_STATUS_COMPLETED) {
        if (a->outcome != COG_OK) return false;
    } else if (!cli_wire_result(a->outcome) || a->outcome == COG_OK) return false;
    return a->status >= COG_AGENT_STATUS_COMPLETED || result == 0U;
}
static bool policy_valid(const cog_cli_policy_view_t *p, size_t sid, size_t name,
    const char *root, size_t root_len, uint64_t active)
{
    return p->assistant_node != 0U && p->nonce != 0U && p->deadline_ms != 0U &&
        p->configuration_generation == active && p->call_ordinal <= 15U && name != 0U && name <= 255U &&
        sid == root_len && cli_wire_session(p->session_id, sid) && memcmp(p->session_id, root, sid) == 0;
}
static bool source_disjoint(const void *source, size_t size, const void *output,
    size_t capacity, const size_t *required)
{
    return cli_wire_disjoint(source, size, output, capacity) && cli_wire_disjoint(source, size, required, sizeof(*required));
}
static void projection_tail(cli_wire_writer_t *w, const cog_cli_projection_view_t *v,
    size_t lens[16][3], size_t policy_lens[2])
{
    cli_wire_put(w, v->active_configuration, 8U); cli_wire_put(w, v->published_configuration, 8U);
    cli_wire_put(w, v->agent_count, 2U);
    for (size_t i = 0U; i < v->agent_count; ++i) {
        const cog_cli_agent_view_t *a = &v->agents[i];
        cli_wire_put(w, a->agent_id, 8U); cli_wire_put(w, a->execution_epoch, 8U);
        cli_wire_put(w, a->group_id, 8U); cli_wire_put(w, a->configuration_generation, 8U);
        cli_wire_string(w, a->session_id, lens[i][0]); cli_wire_put(w, (uint64_t)a->status, 1U);
        cli_wire_put(w, (uint32_t)a->outcome, 4U); cli_wire_string(w, a->status_text, lens[i][1]);
        cli_wire_string(w, a->result_text, lens[i][2]);
    }
    cli_wire_put(w, v->policy_count, 2U);
    for (size_t i = 0U; i < v->policy_count; ++i) {
        const cog_cli_policy_view_t *p = &v->policies[i];
        cli_wire_string(w, p->session_id, policy_lens[0]); cli_wire_put(w, p->assistant_node, 8U);
        cli_wire_put(w, p->call_ordinal, 2U); cli_wire_put(w, p->nonce, 8U);
        cli_wire_put(w, p->configuration_generation, 8U); cli_wire_put(w, p->deadline_ms, 8U);
        cli_wire_string(w, p->tool_name, policy_lens[1]);
    }
}
cog_result_t cog_cli_projection_encode(const cog_cli_projection_view_t *view,
    void *output, size_t capacity, size_t *required)
{
    if (view == NULL || required == NULL || (output == NULL && capacity != 0U)) return COG_ERROR_NULL_PTR;
    if (view->active_configuration == 0U || view->published_configuration < view->active_configuration ||
        view->display.through_sequence == 0U || view->agent_count > 16U || view->policy_count > 1U ||
        (view->agents == NULL && view->agent_count != 0U) || (view->policies == NULL && view->policy_count != 0U)) return COG_ERROR_INVALID_ARG;
    size_t sid, display_bytes = 0U;
    if (!cli_wire_cstr(view->display.session_id, 127U, &sid) || !cli_wire_session(view->display.session_id, sid)) return COG_ERROR_INVALID_ARG;
    cog_result_t rc = cog_cli_display_snapshot_encode(&view->display, NULL, 0U, &display_bytes);
    if (rc != COG_ERROR_OVERFLOW) return rc;
    if (display_bytes > UINT32_MAX || view->display.item_count > SIZE_MAX / sizeof(*view->display.items)) return COG_ERROR_LIMIT;
    size_t lens[16][3] = {{0}}, policy_lens[2] = {0};
    if (!cli_wire_disjoint(output, capacity, required, sizeof(*required)) ||
        !source_disjoint(view, sizeof(*view), output, capacity, required) ||
        !source_disjoint(view->display.session_id, sid + 1U, output, capacity, required) ||
        !source_disjoint(view->display.items, view->display.item_count * sizeof(*view->display.items), output, capacity, required) ||
        !source_disjoint(view->agents, view->agent_count * sizeof(*view->agents), output, capacity, required) ||
        !source_disjoint(view->policies, view->policy_count * sizeof(*view->policies), output, capacity, required)) return COG_ERROR_INVALID_ARG;
    for (size_t i = 0U; i < view->display.item_count; ++i)
        if (!source_disjoint(view->display.items[i].text, view->display.items[i].text_len, output, capacity, required)) return COG_ERROR_INVALID_ARG;
    for (size_t i = 0U; i < view->agent_count; ++i) {
        const cog_cli_agent_view_t *a = &view->agents[i];
        if (!cli_wire_cstr(a->session_id, 127U, &lens[i][0]) ||
            !cli_wire_cstr(a->status_text, UINT32_MAX, &lens[i][1]) ||
            !cli_wire_cstr(a->result_text, UINT32_MAX, &lens[i][2]) || !agent_valid(a, lens[i][0], lens[i][2]) ||
            (i != 0U && a->agent_id <= view->agents[i - 1U].agent_id)) return COG_ERROR_INVALID_ARG;
        if (!source_disjoint(a->session_id, lens[i][0] + 1U, output, capacity, required) ||
            !source_disjoint(a->status_text, lens[i][1] + 1U, output, capacity, required) ||
            !source_disjoint(a->result_text, lens[i][2] + 1U, output, capacity, required)) return COG_ERROR_INVALID_ARG;
    }
    if (view->policy_count != 0U) {
        const cog_cli_policy_view_t *p = view->policies;
        if (!cli_wire_cstr(p->session_id, 127U, &policy_lens[0]) ||
            !cli_wire_cstr(p->tool_name, 255U, &policy_lens[1]) ||
            !policy_valid(p, policy_lens[0], policy_lens[1], view->display.session_id, sid, view->active_configuration)) return COG_ERROR_INVALID_ARG;
        if (!source_disjoint(p->session_id, policy_lens[0] + 1U, output, capacity, required) ||
            !source_disjoint(p->tool_name, policy_lens[1] + 1U, output, capacity, required)) return COG_ERROR_INVALID_ARG;
    }
    cli_wire_writer_t w = {NULL, 8U, true};
    cli_wire_bytes(&w, NULL, display_bytes); projection_tail(&w, view, lens, policy_lens);
    if (!w.valid) return COG_ERROR_LIMIT;
    *required = w.used;
    if (capacity < w.used) return COG_ERROR_OVERFLOW;
    w = (cli_wire_writer_t){output, 0U, true};
    cli_wire_bytes(&w, "CGV1", 4U); cli_wire_put(&w, display_bytes, 4U);
    size_t ignored;
    /* The full count/validation pass above made this nonfallible for unchanged input. */
    rc = cog_cli_display_snapshot_encode(&view->display, w.bytes + w.used, display_bytes, &ignored);
    if (rc != COG_OK) return rc;
    w.used += display_bytes; projection_tail(&w, view, lens, policy_lens);
    return COG_OK;
}

cog_result_t cog_cli_projection_decode(const void *input, size_t input_len,
    cog_cli_projection_view_t *view, cog_cli_display_item_t *items,
    size_t item_capacity, cog_cli_agent_view_t *agents, size_t agent_capacity,
    cog_cli_policy_view_t *policies, size_t policy_capacity, char *storage,
    size_t capacity, size_t *required_items, size_t *required_agents,
    size_t *required_policies, size_t *required_storage)
{
    if (input == NULL || view == NULL || required_items == NULL || required_agents == NULL ||
        required_policies == NULL || required_storage == NULL || (items == NULL && item_capacity != 0U) ||
        (agents == NULL && agent_capacity != 0U) || (policies == NULL && policy_capacity != 0U) ||
        (storage == NULL && capacity != 0U)) return COG_ERROR_NULL_PTR;
    if (input_len < 4U || memcmp(input, "CGV", 3U) != 0) return COG_ERROR_PROTOCOL;
    const unsigned char *bytes = input;
    if (bytes[3] != '1') return COG_ERROR_UNSUPPORTED;
    cli_wire_reader_t r = {bytes, input_len, 4U, true};
    size_t display_bytes = (size_t)cli_wire_get(&r, 4U);
    const unsigned char *display = cli_wire_take(&r, display_bytes);
    if (!r.valid) return COG_ERROR_PROTOCOL;
    cog_cli_display_snapshot_t snapshot;
    size_t item_count = 0U, display_text = 0U;
    cog_result_t rc = cog_cli_display_snapshot_decode(display, display_bytes, &snapshot,
        NULL, 0U, NULL, 0U, &item_count, &display_text);
    if (rc != COG_ERROR_OVERFLOW) return COG_ERROR_PROTOCOL;
    /* Existing CGD1 validation above is nonwriting. Extract only its immutable
       anchor/ID here; a later malformed agent/policy must not touch caller rows. */
    cli_wire_reader_t d = {display, display_bytes, 4U, true};
    size_t root_len = (size_t)cli_wire_get(&d, 1U);
    const char *root = (const char *)cli_wire_take(&d, root_len);
    uint64_t generation = cli_wire_get(&d, 8U), sequence = cli_wire_get(&d, 8U);
    if (!cli_wire_session(root, root_len) || sequence == 0U) return COG_ERROR_PROTOCOL;
    cog_cli_projection_view_t v = {0};
    v.active_configuration = cli_wire_get(&r, 8U); v.published_configuration = cli_wire_get(&r, 8U);
    v.agent_count = (size_t)cli_wire_get(&r, 2U);
    if (!r.valid || v.active_configuration == 0U || v.published_configuration < v.active_configuration || v.agent_count > 16U) return COG_ERROR_PROTOCOL;
    cog_cli_agent_view_t parsed_agents[16];
    cog_cli_policy_view_t parsed_policy = {0};
    size_t lens[16][3] = {{0}}, policy_lens[2] = {0}, need = display_text;
    for (size_t i = 0U; i < v.agent_count; ++i) {
        cog_cli_agent_view_t *a = &parsed_agents[i];
        *a = (cog_cli_agent_view_t){0};
        a->agent_id = cli_wire_get(&r, 8U); a->execution_epoch = cli_wire_get(&r, 8U);
        a->group_id = cli_wire_get(&r, 8U); a->configuration_generation = cli_wire_get(&r, 8U);
        a->session_id = cli_wire_read_string(&r, &lens[i][0]);
        a->status = (cog_agent_status_t)cli_wire_get(&r, 1U); a->outcome = cli_wire_signed(cli_wire_get(&r, 4U));
        a->status_text = cli_wire_read_string(&r, &lens[i][1]); a->result_text = cli_wire_read_string(&r, &lens[i][2]);
        if (!r.valid || !agent_valid(a, lens[i][0], lens[i][2]) || (i != 0U && a->agent_id <= parsed_agents[i - 1U].agent_id)) return COG_ERROR_PROTOCOL;
        for (size_t j = 0U; j < 3U; ++j) if (!cli_wire_add(&need, lens[i][j]) || !cli_wire_add(&need, 1U)) return COG_ERROR_PROTOCOL;
    }
    v.policy_count = (size_t)cli_wire_get(&r, 2U);
    if (!r.valid || v.policy_count > 1U) return COG_ERROR_PROTOCOL;
    if (v.policy_count != 0U) {
        parsed_policy.session_id = cli_wire_read_string(&r, &policy_lens[0]);
        parsed_policy.assistant_node = cli_wire_get(&r, 8U);
        parsed_policy.call_ordinal = (uint16_t)cli_wire_get(&r, 2U);
        parsed_policy.nonce = cli_wire_get(&r, 8U); parsed_policy.configuration_generation = cli_wire_get(&r, 8U);
        parsed_policy.deadline_ms = cli_wire_get(&r, 8U);
        parsed_policy.tool_name = cli_wire_read_string(&r, &policy_lens[1]);
        if (!r.valid || !policy_valid(&parsed_policy, policy_lens[0], policy_lens[1], root, root_len, v.active_configuration)) return COG_ERROR_PROTOCOL;
        for (size_t j = 0U; j < 2U; ++j) if (!cli_wire_add(&need, policy_lens[j]) || !cli_wire_add(&need, 1U)) return COG_ERROR_PROTOCOL;
    }
    if (!r.valid || r.used != input_len || item_capacity > SIZE_MAX / sizeof(*items) ||
        agent_capacity > SIZE_MAX / sizeof(*agents) || policy_capacity > SIZE_MAX / sizeof(*policies)) return COG_ERROR_PROTOCOL;
    const cli_wire_region_t regions[] = {{input, input_len}, {view, sizeof(*view)},
        {items, item_capacity * sizeof(*items)}, {agents, agent_capacity * sizeof(*agents)},
        {policies, policy_capacity * sizeof(*policies)}, {storage, capacity},
        {required_items, sizeof(*required_items)}, {required_agents, sizeof(*required_agents)},
        {required_policies, sizeof(*required_policies)}, {required_storage, sizeof(*required_storage)}};
    if (!cli_wire_regions(regions, 10U)) return COG_ERROR_PROTOCOL;
    *required_items = item_count; *required_agents = v.agent_count;
    *required_policies = v.policy_count; *required_storage = need;
    if (item_capacity < item_count || agent_capacity < v.agent_count || policy_capacity < v.policy_count || capacity < need) return COG_ERROR_OVERFLOW;
    size_t ignored_items, ignored_text;
    rc = cog_cli_display_snapshot_decode(display, display_bytes, &v.display, items,
        item_capacity, storage, display_text, &ignored_items, &ignored_text);
    if (rc != COG_OK) return rc;
    char *out = storage + display_text;
    for (size_t i = 0U; i < v.agent_count; ++i) {
        cog_cli_agent_view_t a = parsed_agents[i];
        a.session_id = cli_wire_copy(&out, a.session_id, lens[i][0]);
        a.status_text = cli_wire_copy(&out, a.status_text, lens[i][1]);
        a.result_text = cli_wire_copy(&out, a.result_text, lens[i][2]); agents[i] = a;
    }
    if (v.policy_count != 0U) {
        parsed_policy.session_id = cli_wire_copy(&out, parsed_policy.session_id, policy_lens[0]);
        parsed_policy.tool_name = cli_wire_copy(&out, parsed_policy.tool_name, policy_lens[1]); policies[0] = parsed_policy;
    }
    v.agents = agents; v.policies = policies;
    /* Decode above preserves this exact nonzero canonical anchor, never a raw app cursor. */
    (void)generation;
    *view = v;
    return COG_OK;
}
