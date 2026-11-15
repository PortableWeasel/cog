/**
 * @file endpoint.c
 * @brief Endpoint and model registry with static dispatch tables.
 *
 * Implements the public contract in <cog/endpoint.h>.  Endpoint and model
 * tables are read-only static const arrays supplied by the caller; this
 * module only searches them.  There is no dynamic registration and no
 * global mutable state.
 *
 * The lookup functions treat a malformed table (a non-zero count with a
 * NULL backing array) as COG_ERROR_INVALID_ARG rather than dereferencing
 * a NULL pointer, so a bad table fails closed and loudly.
 */

#include "cog/endpoint.h"

#include <string.h>

cog_result_t cog_endpoint_table_lookup(const cog_endpoint_table_t *table,
                                       const char *name,
                                       const cog_endpoint_t **out_endpoint)
{
    if (table == NULL || name == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (table->count != 0U && table->endpoints == NULL) {
        return COG_ERROR_INVALID_ARG;
    }
    size_t i;
    for (i = 0U; i < table->count; i++) {
        const cog_endpoint_t *ep = &table->endpoints[i];
        if (ep->name != NULL && strcmp(ep->name, name) == 0) {
            if (out_endpoint != NULL) {
                *out_endpoint = ep;
            }
            return COG_OK;
        }
    }
    return COG_ERROR_NOT_FOUND;
}

cog_result_t cog_endpoint_model_lookup(const cog_endpoint_t *endpoint,
                                       const char *id,
                                       const cog_model_t **out_model)
{
    if (endpoint == NULL || endpoint->models == NULL || id == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (endpoint->models->count != 0U && endpoint->models->models == NULL) {
        return COG_ERROR_INVALID_ARG;
    }
    size_t i;
    for (i = 0U; i < endpoint->models->count; i++) {
        const cog_model_t *m = &endpoint->models->models[i];
        if (m->id != NULL && strcmp(m->id, id) == 0) {
            if (out_model != NULL) {
                *out_model = m;
            }
            return COG_OK;
        }
    }
    return COG_ERROR_NOT_FOUND;
}

bool cog_model_has_capability(const cog_model_t *model,
                              cog_model_capability_t capability)
{
    if (model == NULL) {
        return false;
    }
    return (model->capabilities & (uint32_t)capability) != 0U;
}

cog_result_t cog_endpoint_table_select_by_model(const cog_endpoint_table_t *table,
                                                const char *model_id,
                                                const cog_endpoint_t **out_endpoint,
                                                const cog_model_t **out_model)
{
    if (table == NULL || model_id == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (table->count != 0U && table->endpoints == NULL) {
        return COG_ERROR_INVALID_ARG;
    }
    size_t i;
    for (i = 0U; i < table->count; i++) {
        const cog_endpoint_t *ep = &table->endpoints[i];
        if (ep == NULL || ep->models == NULL) {
            continue;
        }
        if (ep->models->count != 0U && ep->models->models == NULL) {
            continue;
        }
        size_t j;
        for (j = 0U; j < ep->models->count; j++) {
            const cog_model_t *m = &ep->models->models[j];
            if (m->id != NULL && strcmp(m->id, model_id) == 0) {
                if (out_endpoint != NULL) {
                    *out_endpoint = ep;
                }
                if (out_model != NULL) {
                    *out_model = m;
                }
                return COG_OK;
            }
        }
    }
    return COG_ERROR_NOT_FOUND;
}
