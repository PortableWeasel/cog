/**
 * @file provider.h
 * @brief Public umbrella header for cog-provider.
 *
 * cog-provider abstracts LLM provider endpoints behind a static vtable
 * and exposes an endpoint/model registry.  A consumer of this library
 * needs only:
 *
 * @code
 * #include "cog/provider.h"
 * @endcode
 *
 * All state is caller-allocated and passed by pointer.  Provider
 * instances are created by @c cog_provider_connect and destroyed by
 * @c cog_provider_disconnect; request and response buffers are supplied
 * by the caller.
 *
 * Threading model: endpoint and model tables are read-only after
 * construction and may be shared across threads.  A single
 * @c cog_provider_t instance is not thread-safe and must be driven by
 * one thread at a time.
 */
#ifndef COG_PROVIDER_H
#define COG_PROVIDER_H

#include <cog/endpoint.h>
#include <cog/message.h>
#include <cog/provider_request.h>
#include <cog/provider_vtable.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif /* COG_PROVIDER_H */
