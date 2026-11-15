/** @file provider.c @brief Validated public static provider dispatch. */
#include <cog/provider.h>
#include <math.h>
#include <string.h>

static bool complete(const cog_provider_vtable_t *v)
{
    return v != NULL && v->connect != NULL && v->disconnect != NULL &&
           v->send != NULL && v->receive != NULL && v->poll != NULL &&
           v->cancel != NULL && v->next != NULL && v->error != NULL &&
           v->preflight != NULL;
}
cog_result_t cog_provider_request_validate(const cog_provider_request_t *r)
{
    if (r == NULL)
        return COG_ERROR_NULL_PTR;
    if (r->model_id == NULL || r->model_id[0] == '\0' ||
        (r->messages == NULL && r->message_count != 0U) ||
        (r->tools_json == NULL && r->tools_json_len != 0U) ||
        !isfinite(r->temperature) || r->temperature < 0.0f ||
        r->temperature > 2.0f)
        return COG_ERROR_INVALID_ARG;
    cog_message_t model = {.role = COG_MESSAGE_ROLE_USER,
                           .content = r->model_id,
                           .content_len = strlen(r->model_id)};
    if (cog_message_validate(&model) != COG_OK)
        return COG_ERROR_INVALID_ARG;
    for (size_t i = 0; i < r->message_count; ++i)
    {
        cog_result_t rc = cog_message_validate(&r->messages[i]);
        if (rc != COG_OK)
            return rc;
    }
    return COG_OK;
}
cog_result_t cog_provider_response_validate(const cog_provider_response_t *r)
{
    if (r == NULL)
        return COG_ERROR_NULL_PTR;
    return r->content == NULL && r->content_capacity != 0U
               ? COG_ERROR_INVALID_ARG
               : COG_OK;
}
cog_result_t cog_provider_connect(const cog_endpoint_t *e, cog_provider_t **out)
{
    if (out == NULL)
        return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (e == NULL)
        return COG_ERROR_NULL_PTR;
    if (!complete(e->vtable))
        return COG_ERROR_INVALID_ARG;
    cog_result_t rc = e->vtable->connect(e, out);
    if (rc != COG_OK)
        *out = NULL;
    return rc;
}
cog_result_t cog_provider_disconnect(cog_provider_t *p)
{
    if (p == NULL)
        return COG_OK;
    if (!complete(p->vtable))
        return COG_ERROR_INVALID_ARG;
    return p->vtable->disconnect(p);
}
/* Result IDs belong to the immediately preceding assistant call group. Scan
 * its already-recorded results instead of allocating an ID set. Closed groups
 * may reuse IDs; a partial group cannot be crossed by another message. */
static cog_result_t history_validate(const cog_provider_request_t *r,
                                     cog_provider_preflight_stage_t stage)
{
    const cog_message_t *group = NULL;
    size_t group_index = 0U, resolved = 0U;
    for (size_t i = 0U; i < r->message_count; ++i)
    {
        const cog_message_t *m = &r->messages[i];
        if (m->status != COG_MESSAGE_COMPLETE)
            return COG_ERROR_INVALID_STATE;
        if (m->role == COG_MESSAGE_ROLE_TOOL)
        {
            if (group == NULL)
                return COG_ERROR_INVALID_STATE;
            size_t call = 0U;
            while (call < group->tool_call_count &&
                   strcmp(m->tool_call_id, group->tool_calls[call].id) != 0)
                ++call;
            if (call == group->tool_call_count)
                return COG_ERROR_INVALID_STATE;
            for (size_t j = group_index + 1U; j < i; ++j)
                if (strcmp(m->tool_call_id, r->messages[j].tool_call_id) == 0)
                    return COG_ERROR_INVALID_STATE;
            ++resolved;
        }
        else
        {
            if (group != NULL && resolved != group->tool_call_count)
                return COG_ERROR_INVALID_STATE;
            group = NULL;
            resolved = 0U;
            if (m->tool_call_count != 0U)
            {
                group = m;
                group_index = i;
            }
        }
    }
    bool pending = group != NULL && resolved < group->tool_call_count;
    return pending == (stage == COG_PROVIDER_PREFLIGHT_PENDING_TOOLS)
               ? COG_OK : COG_ERROR_INVALID_STATE;
}
cog_result_t cog_provider_preflight(cog_provider_t *p,
    const cog_provider_request_t *r, cog_provider_preflight_stage_t stage)
{
    if (p == NULL || r == NULL)
        return COG_ERROR_NULL_PTR;
    if (!complete(p->vtable) ||
        (stage != COG_PROVIDER_PREFLIGHT_COMPLETE &&
         stage != COG_PROVIDER_PREFLIGHT_PENDING_TOOLS))
        return COG_ERROR_INVALID_ARG;
    cog_result_t rc = cog_provider_request_validate(r);
    if (rc == COG_OK)
        rc = history_validate(r, stage);
    return rc == COG_OK ? p->vtable->preflight(p, r, stage) : rc;
}
cog_result_t cog_provider_send(cog_provider_t *p,
                               const cog_provider_request_t *r)
{
    if (p == NULL || r == NULL)
        return COG_ERROR_NULL_PTR;
    if (!complete(p->vtable))
        return COG_ERROR_INVALID_ARG;
    cog_result_t rc = cog_provider_preflight(
        p, r, COG_PROVIDER_PREFLIGHT_COMPLETE);
    return rc == COG_OK ? p->vtable->send(p, r) : rc;
}
cog_result_t cog_provider_receive(cog_provider_t *p, cog_provider_response_t *r)
{
    if (p == NULL || r == NULL)
        return COG_ERROR_NULL_PTR;
    if (!complete(p->vtable))
        return COG_ERROR_INVALID_ARG;
    cog_result_t rc = cog_provider_response_validate(r);
    return rc == COG_OK ? p->vtable->receive(p, r) : rc;
}
cog_result_t cog_provider_poll(cog_provider_t *p, uint64_t now,
                               uint64_t *deadline)
{
    if (p == NULL || deadline == NULL)
        return COG_ERROR_NULL_PTR;
    if (!complete(p->vtable))
        return COG_ERROR_INVALID_ARG;
    return p->vtable->poll(p, now, deadline);
}
cog_result_t cog_provider_cancel(cog_provider_t *p)
{
    if (p == NULL)
        return COG_ERROR_NULL_PTR;
    if (!complete(p->vtable))
        return COG_ERROR_INVALID_ARG;
    return p->vtable->cancel(p);
}
cog_result_t cog_provider_next(cog_provider_t *p, cog_provider_delta_t *d,
                               void *storage, size_t cap, size_t *required)
{
    if (p == NULL || d == NULL || required == NULL)
        return COG_ERROR_NULL_PTR;
    if (storage == NULL && cap != 0U)
        return COG_ERROR_INVALID_ARG;
    if (!complete(p->vtable))
        return COG_ERROR_INVALID_ARG;
    return p->vtable->next(p, d, storage, cap, required);
}
cog_result_t cog_provider_get_error(const cog_provider_t *p,
                                    cog_provider_error_t *e)
{
    if (p == NULL || e == NULL)
        return COG_ERROR_NULL_PTR;
    if (!complete(p->vtable))
        return COG_ERROR_INVALID_ARG;
    return p->vtable->error(p, e);
}
