/**
 * @file provider.c
 * @brief Provider abstraction: request/response validation and static
 *        vtable dispatch wrappers.
 *
 * Implements the public contract in <cog/provider_request.h> and
 * <cog/provider_vtable.h>.  This module owns no concrete provider
 * implementation; it validates request/response structs and dispatches
 * connect/send/receive/disconnect to the static vtable referenced by the
 * endpoint (for connect) or embedded in the provider instance (for the
 * other three operations).
 *
 * The provider instance layout is defined in provider_internal.h: the
 * first member of the opaque @c cog_provider_t is a pointer to the static
 * vtable that created it.  Provider implementations must embed that
 * pointer first so the wrappers can reach the vtable.
 *
 * No global mutable state.  All state is caller-allocated and passed by
 * pointer.
 */

#include "cog/provider.h"

#include "provider_internal.h"

/* ------------------------------------------------------------------ */
/* Request / response validation                                       */
/* ------------------------------------------------------------------ */

cog_result_t cog_provider_request_validate(const cog_provider_request_t *request)
{
    if (request == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (request->model_id == NULL) {
        return COG_ERROR_INVALID_ARG;
    }
    if (request->messages == NULL && request->message_count != 0U) {
        return COG_ERROR_INVALID_ARG;
    }
    if (request->tools_json == NULL && request->tools_json_len != 0U) {
        return COG_ERROR_INVALID_ARG;
    }
    return COG_OK;
}

cog_result_t cog_provider_response_validate(const cog_provider_response_t *response)
{
    if (response == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (response->content == NULL && response->content_capacity != 0U) {
        return COG_ERROR_INVALID_ARG;
    }
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Static vtable dispatch wrappers                                     */
/* ------------------------------------------------------------------ */

cog_result_t cog_provider_connect(const cog_endpoint_t *endpoint,
                                  cog_provider_t **out_provider)
{
    if (endpoint == NULL || endpoint->vtable == NULL || out_provider == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    return endpoint->vtable->connect(endpoint, out_provider);
}

cog_result_t cog_provider_disconnect(cog_provider_t *provider)
{
    if (provider == NULL) {
        return COG_OK; /* no-op */
    }
    return provider->vtable->disconnect(provider);
}

cog_result_t cog_provider_send(cog_provider_t *provider,
                               const cog_provider_request_t *request)
{
    if (provider == NULL || request == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    return provider->vtable->send(provider, request);
}

cog_result_t cog_provider_receive(cog_provider_t *provider,
                                  cog_provider_response_t *response)
{
    if (provider == NULL || response == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    return provider->vtable->receive(provider, response);
}
