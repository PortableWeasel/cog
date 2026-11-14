/**
 * @file endpoint.h
 * @brief Endpoint and model registry with static dispatch tables.
 *
 * Endpoints describe how to reach an LLM provider (base URL, API
 * version, credentials) and which provider implementation handles the
 * connection.  Each endpoint references a static table of models that
 * the endpoint can serve.  All tables are read-only after construction
 * and resolved at compile or link time.
 *
 * Threading model: endpoint and model tables are read-only after
 * construction and may be shared by multiple provider instances and
 * threads.  Individual provider instances are not thread-safe.
 */
#ifndef COG_ENDPOINT_H
#define COG_ENDPOINT_H

#include <cog/provider_vtable.h>
#include <cog/result.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Capability flags for a model.
 */
typedef enum cog_model_capability {
    COG_MODEL_CAPABILITY_STREAMING = 0x01u, /**< Supports streaming responses. */
    COG_MODEL_CAPABILITY_TOOLS     = 0x02u, /**< Supports tool/function calls. */
    COG_MODEL_CAPABILITY_VISION    = 0x04u, /**< Accepts image inputs. */
} cog_model_capability_t;

/**
 * @brief Descriptor for a single model available through an endpoint.
 *
 * Rows are assembled into a static const array and referenced by an
 * endpoint descriptor.
 */
typedef struct cog_model {
    const char *id;          /**< Endpoint-specific model identifier;
                                  non-NULL, NUL-terminated, unique within
                                  the endpoint. */
    const char *name;        /**< Human-readable name; may be NULL. */
    uint32_t context_window; /**< Maximum context length in tokens; zero
                                  if unknown. */
    uint32_t capabilities;   /**< Bitmask of @c cog_model_capability_t. */
} cog_model_t;

/**
 * @brief Static model table wrapper.
 *
 * The underlying array must remain valid for the lifetime of any
 * endpoint that references it.
 */
typedef struct cog_model_table {
    const cog_model_t *models; /**< Pointer to first model descriptor. */
    size_t count;              /**< Number of descriptors.  May be zero. */
} cog_model_table_t;

/**
 * @brief Descriptor for a single provider endpoint.
 *
 * Rows are assembled into a static const array and supplied to the
 * registry lookup functions.  The vtable and model table must remain
 * valid for the lifetime of any code that references the endpoint.
 */
typedef struct cog_endpoint {
    const char *name;        /**< Endpoint name used for dispatch;
                                  non-NULL, NUL-terminated, unique within
                                  the table. */
    const char *base_url;    /**< Provider base URL; may be NULL if the
                                  provider does not use one. */
    const char *api_version; /**< Provider API version; may be NULL. */
    const char *api_key;     /**< Credential; may be NULL.  Provider-specific
                                  interpretation. */
    const cog_provider_vtable_t *vtable; /**< Provider implementation. */
    const cog_model_table_t *models;     /**< Models served by this endpoint. */
} cog_endpoint_t;

/**
 * @brief Static endpoint table wrapper.
 *
 * The underlying array must remain valid for the lifetime of any code
 * that references it.
 */
typedef struct cog_endpoint_table {
    const cog_endpoint_t *endpoints; /**< Pointer to first endpoint descriptor. */
    size_t count;                    /**< Number of descriptors.  May be zero. */
} cog_endpoint_table_t;

/**
 * @brief Look up an endpoint by name.
 *
 * @param[in]  table       Endpoint table to search.
 * @param[in]  name        Endpoint name to look up; NUL-terminated.
 * @param[out] out_endpoint Receives a pointer to the matching descriptor on
 *                          success; unchanged on failure.  May be NULL if
 *                          only presence is required.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p table or @p name is NULL.
 * @return COG_ERROR_NOT_FOUND if no endpoint matches @p name.
 *
 * Threading: the table must not be modified during the call.
 */
cog_result_t cog_endpoint_table_lookup(const cog_endpoint_table_t *table,
                                       const char *name,
                                       const cog_endpoint_t **out_endpoint);

/**
 * @brief Look up a model by id within an endpoint's model table.
 *
 * @param[in]  endpoint   Endpoint whose models are searched.
 * @param[in]  id         Model id to look up; NUL-terminated.
 * @param[out] out_model  Receives a pointer to the matching descriptor on
 *                        success; unchanged on failure.  May be NULL if
 *                        only presence is required.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p endpoint, @p endpoint->models, or
 *         @p id is NULL.
 * @return COG_ERROR_NOT_FOUND if no model matches @p id.
 *
 * Threading: the table must not be modified during the call.
 */
cog_result_t cog_endpoint_model_lookup(const cog_endpoint_t *endpoint,
                                       const char *id,
                                       const cog_model_t **out_model);

/**
 * @brief Return true if a model advertises a given capability.
 *
 * @param[in] model      Model to inspect; may be NULL.
 * @param[in] capability Single capability flag to test.
 * @return true if @p model is non-NULL and has the capability, false
 *         otherwise.
 *
 * Threading: the model must not be modified during the call.
 */
bool cog_model_has_capability(const cog_model_t *model,
                              cog_model_capability_t capability);

#ifdef __cplusplus
}
#endif

#endif /* COG_ENDPOINT_H */
