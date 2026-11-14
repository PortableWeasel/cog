/**
 * @file provider_vtable.h
 * @brief Provider abstraction interface.
 *
 * A provider is a concrete implementation of an LLM endpoint accessed
 * through a static vtable.  The four required operations are connect,
 * send, receive, and disconnect.  Both single-turn and streaming calls
 * are expressed with the same operations: a streaming request sets
 * @c cog_provider_request_t.stream to true and the caller loops on
 * @c cog_provider_receive until @c cog_provider_response_t.is_complete
 * is true.
 *
 * Threading model: a @c cog_provider_t instance is not thread-safe.  All
 * access happens on the thread that owns the provider.  Provider
 * implementations must be re-entrant: distinct instances created by
 * @c cog_provider_connect may be used concurrently, but a single
 * instance may not.
 */
#ifndef COG_PROVIDER_VTABLE_H
#define COG_PROVIDER_VTABLE_H

#include <cog/provider_request.h>
#include <cog/result.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration to break the endpoint/provider include cycle. */
typedef struct cog_endpoint cog_endpoint_t;

/**
 * @brief Opaque provider instance.
 *
 * Created by a provider-specific @c cog_provider_connect_fn and
 * destroyed by the matching @c cog_provider_disconnect_fn.
 */
typedef struct cog_provider cog_provider_t;

/**
 * @brief Create and initialise a provider instance for an endpoint.
 *
 * @param[in]  endpoint    Endpoint to connect to.  Must be non-NULL.
 * @param[out] out_provider Uninitialised pointer; set to the new provider
 *                          on success.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p endpoint or @p out_provider is NULL.
 * @return COG_ERROR_NOMEM if allocation fails.
 * @return Another error code if the endpoint cannot be reached.
 *
 * @post On success, @p *out_provider is a valid provider and the caller
 *       owns it.
 * @post On failure, @p *out_provider is NULL and no resources are held.
 */
typedef cog_result_t (*cog_provider_connect_fn)(const cog_endpoint_t *endpoint,
                                                cog_provider_t **out_provider);

/**
 * @brief Tear down a provider instance.
 *
 * @param[in,out] provider Provider to disconnect; may be NULL (no-op).
 *
 * @post All storage owned by @p provider is released.  @p provider must
 *       not be used after this call.
 */
typedef cog_result_t (*cog_provider_disconnect_fn)(cog_provider_t *provider);

/**
 * @brief Submit a request to the provider.
 *
 * This call initiates the LLM call but does not necessarily wait for the
 * full response.  The caller reads the response with
 * @c cog_provider_receive.
 *
 * @param[in,out] provider Provider instance.
 * @param[in]     request  Request to send.  Must be non-NULL.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p provider or @p request is NULL.
 * @return COG_ERROR_INVALID_STATE if no connection is open.
 * @return Another error code on failure.
 *
 * @pre @p provider has been connected.
 * @pre @p request has been validated with
 *      @c cog_provider_request_validate.
 */
typedef cog_result_t (*cog_provider_send_fn)(cog_provider_t *provider,
                                             const cog_provider_request_t *request);

/**
 * @brief Read the next response or streaming chunk.
 *
 * For single-turn requests this returns the complete response in one
 * call and sets @p response->is_complete to true.  For streaming
 * requests the caller loops until @p response->is_complete is true.
 *
 * @param[in,out] provider Provider instance.
 * @param[in,out] response Caller-provided response buffer.  Must be
 *                         non-NULL and validated with
 *                         @c cog_provider_response_validate.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p provider or @p response is NULL.
 * @return COG_ERROR_INVALID_STATE if no request is in flight.
 * @return COG_ERROR_OVERFLOW if the response does not fit in
 *         @p response->content_capacity.
 * @return COG_ERROR_ABORTED if the stream was cancelled.
 * @return Another error code on failure.
 *
 * @pre @p provider has sent a request.
 * @post On success, @p response contains the next response text or
 *       chunk and indicates whether the response is complete.
 * @post On failure, @p response is unchanged.
 */
typedef cog_result_t (*cog_provider_receive_fn)(cog_provider_t *provider,
                                                cog_provider_response_t *response);

/**
 * @brief Static provider vtable.
 *
 * All function pointers must be non-NULL.  The table is stored in
 * read-only memory and shared by every endpoint that uses the same
 * provider implementation.
 */
typedef struct cog_provider_vtable {
    cog_provider_connect_fn connect;       /**< Create a provider instance. */
    cog_provider_disconnect_fn disconnect; /**< Destroy a provider instance. */
    cog_provider_send_fn send;             /**< Submit a request. */
    cog_provider_receive_fn receive;       /**< Read the next response. */
} cog_provider_vtable_t;

/**
 * @brief Connect to an endpoint using its registered vtable.
 *
 * This is a convenience wrapper that calls
 * @p endpoint->vtable->connect.
 *
 * @param[in]  endpoint    Endpoint to connect to.  Must be non-NULL.
 * @param[out] out_provider Uninitialised pointer; set to the new provider
 *                          on success.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p endpoint, @p endpoint->vtable, or
 *         @p out_provider is NULL.
 * @return Another error code from the provider's connect function.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_provider_connect(const cog_endpoint_t *endpoint,
                                  cog_provider_t **out_provider);

/**
 * @brief Disconnect a provider instance.
 *
 * Convenience wrapper that calls the provider's disconnect function.
 *
 * @param[in,out] provider Provider to disconnect; may be NULL (no-op).
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_provider_disconnect(cog_provider_t *provider);

/**
 * @brief Send a request through a provider instance.
 *
 * Convenience wrapper that calls the provider's send function.
 *
 * @param[in,out] provider Provider instance.
 * @param[in]     request  Request to send.
 * @return Result from the provider's send function.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_provider_send(cog_provider_t *provider,
                               const cog_provider_request_t *request);

/**
 * @brief Receive the next response from a provider instance.
 *
 * Convenience wrapper that calls the provider's receive function.
 *
 * @param[in,out] provider Provider instance.
 * @param[in,out] response Response buffer.
 * @return Result from the provider's receive function.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_provider_receive(cog_provider_t *provider,
                                  cog_provider_response_t *response);

#ifdef __cplusplus
}
#endif

#endif /* COG_PROVIDER_VTABLE_H */
