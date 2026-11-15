/**
 * @file net.h
 * @brief Explicit HTTP/TLS ownership and nonblocking transport service.
 *
 * One owner thread drives a context; none of these APIs are signal-safe.
 * No Cog mutable process-global state, lazy initialization or reference counter.
 * The host explicitly initializes libcurl/c-ares/TLS before any context and
 * cleans up only after ALL process users, including non-Cog users, finish.
 * Dependency initialization/cleanup are host calls, never cog_net_new/free.
 *
 * Admission requires a libcurl build proven c-ares-only: ENABLE_ARES=ON and
 * ENABLE_THREADED_RESOLVER=OFF, runtime ASYNCHDNS plus nonempty ares version,
 * and CURLOPT_DNS_SERVERS support. ASYNCHDNS or ares string alone also occur
 * with hybrid/threaded resolvers and are insufficient. A mismatched or unknown
 * provenance build fails UNSUPPORTED; never silently falls back to threads or
 * synchronous DNS. Local latency ceilings are regression targets, not an OS
 * real-time guarantee. DNS cancellation must not join resolver threads.
 *
 * Constructors reserve bounded Cog-owned slots/copies/rings and own explicit
 * CURLM/easy/header/dependency handles until matching free/request_release.
 * Those handles MAY allocate during service inside libcurl, c-ares and TLS.
 * This is declared constructor-owned third-party runtime allocation, not a
 * claim of constructor-only allocation or a total byte ceiling. Context limits
 * bound Cog memory and transfer count only. No per-context allocator is imposed
 * via curl's process-global allocator callbacks. Hosts requiring a total memory
 * ceiling cannot use this backend; a nonzero dependency_memory_limit is rejected.
 */
#ifndef COG_NET_H
#define COG_NET_H
#include <cog/result.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct cog_net_ctx cog_net_ctx_t;
typedef uint64_t cog_request_id_t;
/** @brief Explicit acknowledgement of dependency allocation semantics. */
typedef enum cog_net_dependency_memory {
    COG_NET_DEPENDENCY_MEMORY_UNSPECIFIED = 0,
    COG_NET_DEPENDENCY_MEMORY_DYNAMIC = 1
} cog_net_dependency_memory_t;

/** @brief Context constructor settings; zero limits select documented defaults. */
typedef struct cog_net_config {
    size_t max_connections; /**< Transfer slots; default 4, includes terminal retained. */
    uint32_t connect_timeout_ms; /**< DNS+TCP+TLS deadline; default 10000 ms. */
    uint32_t stall_timeout_ms; /**< Byte inactivity after response starts; default 30000. */
    size_t max_retries; /**< MUST be zero: retry belongs to the application. */
    uint32_t retry_backoff_ms; /**< MUST be zero: transport never replays a request. */
    bool host_runtime_initialized; /**< Required true; host lifecycle precondition. */
    cog_net_dependency_memory_t dependency_memory; /**< Required DYNAMIC acknowledgement. */
    size_t dependency_memory_limit; /**< Only 0 supported; no total dependency byte bound. */
    size_t max_request_bytes; /**< Copy of method+URL+headers+body/slot; default 1 MiB. */
    size_t max_header_bytes; /**< Per-response header ceiling; default 64 KiB. */
    size_t chunk_bytes; /**< Retained body slice bytes; default 16 KiB. */
    size_t chunks_per_request; /**< Body ring entries; default 8 plus reserved terminal. */
    uint32_t service_quantum; /**< Maximum callback/completion work per service; default 16. */
    const char *ca_bundle; /**< Optional copied trust path; NULL uses system trust. */
} cog_net_config_t;

/** @brief Sanitized per-transfer failure class; HTTP is not a transport failure. */
typedef enum cog_net_error_kind {
    COG_NET_ERROR_NONE = 0,
    COG_NET_ERROR_DNS,
    COG_NET_ERROR_TLS,
    COG_NET_ERROR_CONNECT,
    COG_NET_ERROR_TIMEOUT,
    COG_NET_ERROR_CAPACITY,
    COG_NET_ERROR_CANCELLED,
    COG_NET_ERROR_DEPENDENCY
} cog_net_error_kind_t;

/**
 * @brief Retained response view; peek does not consume it.
 *
 * All pointer members are context-owned, immutable and valid until ack of this
 * sequence or request_release/free, including across service/cancel calls.
 * Sequence starts at 1 per request. Exactly one terminal item reports outcome;
 * status/headers may be available without body, and HTTP 429/5xx are delivered
 * as HTTP responses rather than transport failures. Terminal error strings are
 * sanitized and exclude credentials, URL userinfo/query, request/response body.
 */
typedef struct cog_net_response {
    int http_status;
    const char *headers; /**< Newline key:value, NUL terminated; NULL before headers. */
    const char *chunk; /**< Binary body bytes, not necessarily NUL terminated. */
    size_t chunk_len;
    bool is_complete;
    cog_request_id_t request_id;
    uint64_t sequence;
    cog_result_t outcome; /**< Terminal result; COG_OK for HTTP error responses. */
    cog_net_error_kind_t error_kind;
    const char *error_message;
    uint32_t retry_after_ms; /**< Parsed only when valid; 0 means unknown. */
    bool retry_after_known;
} cog_net_response_t;

/**
 * @brief Reserve a context and validate explicit dependency lifecycle/backend.
 * @param[out] out Set NULL on failure; owned context on success.
 * @param[in] config Required explicit runtime/memory acknowledgement.
 * @return COG_OK; NULL_PTR; INVALID_ARG for invalid/overflowed limits, retries,
 * missing acknowledgement or lifecycle assertion; UNSUPPORTED for backend or
 * memory-limit mismatch; NOMEM for Cog/dependency setup OOM. No resources on
 * failure. Config strings are copied. Owner-thread only, no network effects.
 */
cog_result_t cog_net_new(cog_net_ctx_t **out, const cog_net_config_t *config);
/**
 * @brief Cancel/release all requests and free context-owned handles/workspaces.
 * @param[in,out] ctx NULL no-op. Host runtime must still be live.
 * @note Owner thread, outside callbacks. No DNS joins, peer waits or retry;
 * after return no context callbacks occur. Then host may clean up dependencies.
 */
void cog_net_free(cog_net_ctx_t *ctx);

/**
 * @brief Explicit request constructor: copy all inputs, own a transfer by ID.
 * @param[in,out] ctx Context with a free slot.
 * @param[in] method Nonempty HTTP method; no CR/LF.
 * @param[in] url HTTPS URL; userinfo rejected, TLS peer+hostname always verified.
 * @param[in] headers Optional newline key:value; reject malformed/injected lines.
 * @param[in] body Binary input, NULL iff body_len=0; copied before return.
 * @param[in] body_len Exact bytes.
 * @param[out] out_id Nonzero never-reused context ID; unchanged on failure.
 * @return COG_OK; NULL_PTR; INVALID_ARG; OVERFLOW if slots full; LIMIT if copies
 * exceed reserved capacity; NOMEM for dependency setup. Atomic failure sends
 * nothing and retains no slot. Match each success with request_release().
 * @note Owner thread, no blocking I/O. No redirects/proxy inheritance/retry.
 */
cog_result_t cog_net_request(cog_net_ctx_t *ctx, const char *method,
                             const char *url, const char *headers,
                             const char *body, size_t body_len,
                             cog_request_id_t *out_id);
/**
 * @brief Stop wire activity and reserve one terminal ABORTED after queued bytes.
 * @param[in,out] ctx Context.
 * @param[in] id Request ID; terminal already queued wins without second terminal.
 * @return COG_OK (idempotent for retained known IDs), NULL_PTR, NOT_FOUND.
 * @note Owner thread, no waiting. No future network data accepted; queued data
 * remains valid until ack. Cancellation does not release the constructor slot.
 */
cog_result_t cog_net_cancel(cog_net_ctx_t *ctx, cog_request_id_t id);
/**
 * @brief Explicit request destructor, including active cancellation if needed.
 * @param[in,out] ctx Context.
 * @param[in] id Known ID. Discards retained data, closes dependency easy handle.
 * @return COG_OK, NULL_PTR or NOT_FOUND. Slot reusable only after this call.
 * @note Owner thread; no peer waits. Invalidates all views for this ID.
 */
cog_result_t cog_net_request_release(cog_net_ctx_t *ctx, cog_request_id_t id);
/**
 * @brief Perform bounded work without sleeping or polling with a positive wait.
 * @param[in,out] ctx Context.
 * @param[in] now_ms Nondecreasing caller monotonic milliseconds.
 * @param[out] out_next_deadline_ms Earliest transport timer; UINT64_MAX if none.
 * @return COG_OK; NULL_PTR; INVALID_ARG for backwards clock; fatal driver error.
 * Request errors/OOM become terminal outcomes; driver errors propagate. Full
 * rings pause reads without dropping accepted data. A body callback is split
 * transactionally into available slots or wholly paused; oversized headers or
 * configured per-item ceilings terminate LIMIT. No silent truncation.
 * @note Owner thread. CPU/syscall/dependency work is bounded by quantum, but
 * cannot promise a hard elapsed-time bound under arbitrary OS/TLS allocation.
 */
cog_result_t cog_net_service(cog_net_ctx_t *ctx, uint64_t now_ms,
                             uint64_t *out_next_deadline_ms);
/**
 * @brief Peek the oldest unacknowledged response across requests fairly.
 * @param[in,out] ctx Context; does not drive/wait for network.
 * @param[out] out_response Borrowed stable view, unchanged on failure.
 * @return COG_OK; AGAIN when none ready; NULL_PTR.
 * @note Repeated peek returns the same selected item until ack/release, even
 * across service calls. Owner thread. No allocation or output consumption.
 */
cog_result_t cog_net_peek(cog_net_ctx_t *ctx, cog_net_response_t *out_response);
/**
 * @brief Consume exactly the currently peeked sequence after downstream copy.
 * @param[in,out] ctx Context.
 * @param[in] id Selected request.
 * @param[in] sequence Selected sequence.
 * @return COG_OK; NULL_PTR; NOT_FOUND for unknown ID; INVALID_STATE for stale or
 * out-of-order acknowledgement. Failure consumes nothing. Owner thread.
 */
cog_result_t cog_net_ack(cog_net_ctx_t *ctx, cog_request_id_t id, uint64_t sequence);
/**
 * @brief Legacy alias for peek; caller MUST ack before obtaining the next item.
 * @param[in,out] ctx Context.
 * @param[out] out_response Same view/lifetime as peek.
 * @return Same results as cog_net_peek; terminal outcome is in response.
 * @note No implicit transport service. Owner thread.
 */
cog_result_t cog_net_poll(cog_net_ctx_t *ctx, cog_net_response_t *out_response);
/**
 * @brief Legacy optional wake descriptor; never assumes one fd represents curl.
 * @param[in] ctx Context.
 * @return Wake fd if supported, otherwise -1. Correct event loops use the core
 * bounded idle service hook and returned deadline; -1 is not a transport error.
 * @note Owner thread; borrowed, never close it.
 */
int cog_net_fd(const cog_net_ctx_t *ctx);
/** @brief Active wire transfers, excluding retained terminal slots.
 * @param[in] ctx Context or NULL.
 * @return Count (zero for NULL). Owner thread, no allocation.
 */
size_t cog_net_active_count(const cog_net_ctx_t *ctx);
/**
 * @brief Copy active wire IDs without partial capacity success.
 * @param[in] ctx Context.
 * @param[out] out_ids Array; NULL iff max_ids=0.
 * @param[in] max_ids Entries available.
 * @param[out] out_count Required entry count on OK/OVERFLOW.
 * @return COG_OK; OVERFLOW (no array writes); NULL_PTR. Owner thread.
 */
cog_result_t cog_net_active_ids(const cog_net_ctx_t *ctx,
                                cog_request_id_t *out_ids, size_t max_ids,
                                size_t *out_count);
/**
 * @brief Query whether a retained ID still has wire work.
 * @param[in] ctx Context.
 * @param[in] id Known constructor ID.
 * @param[out] out_active False after terminal/cancel; unchanged on failure.
 * @return COG_OK; NULL_PTR; NOT_FOUND after release/unknown ID. Owner thread.
 */
cog_result_t cog_net_request_status(const cog_net_ctx_t *ctx,
                                    cog_request_id_t id, bool *out_active);
#ifdef __cplusplus
}
#endif
#endif /* COG_NET_H */
