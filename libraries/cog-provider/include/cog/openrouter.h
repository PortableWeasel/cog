/**
 * @file openrouter.h
 * @brief Explicit composition for the OpenRouter Chat Completions adapter.
 *
 * Private codec: pinned yyjson with constructor-reserved allocator pools,
 * strict UTF-8/JSON, duplicate keys rejected at every nesting level, and
 * quote/escape-aware depth preflight. Unknown finish reasons fail closed.
 * One completion choice. No parser/writer default allocator fallback.
 * Threading: one owner thread, including its borrowed cog-net context.
 */
#ifndef COG_OPENROUTER_H
#define COG_OPENROUTER_H
#include <cog/provider_vtable.h>
#include <cog/net.h>
#ifdef __cplusplus
extern "C" {
#endif
/** @brief Endpoint implementation_config for the static adapter below. */
typedef struct cog_openrouter_config {
    cog_net_ctx_t *net; /**< Borrowed until disconnect; host dependency already live. Multiple adapters
       may share this context on ONE owner thread; each owns/releases only its
       own request ID and drains it with cog_net_peek_request. Poll may service
       the shared transport, but never ack/consume another adapter's bytes. */
    cog_provider_limits_t limits; /**< Explicit nonzero reservation ceilings. */
    uint32_t service_quantum; /**< Max decoded events per poll; zero means 16. */
} cog_openrouter_config_t;
/**
 * @brief Static adapter implementation, no implicit global initialization.
 *
 * Generic preflight validates the complete known selected request in reserved
 * parser/writer storage without net/service calls or accepting a turn. Both
 * stages apply the same pinned namespace/json-v1 syntax and representation
 * rules below; pending-tool preflight does not send an unresolved request.
 * No preflight result changes retained output or the last terminal error.
 * This includes prior get_error message bytes and queued cancellation/FINISH;
 * active instances refuse preflight until FINISH is consumed. Unknown model,
 * generation/capability mismatch and writer/parser limits are checked locally.
 * No synthetic tool result is needed for the permitted pending last group.
 *
 * The first release accepts limits.max_json_depth in 1..256; connect returns
 * INVALID_ARG for a larger value. Depth preflight bounds recursive duplicate
 * and opaque-value equality traversal before strict parsing/copying.
 * endpoint.base_url must be HTTPS, api_key resolved and nonempty, and config
 * non-NULL. Credential copied only into owned in-flight request headers and
 * cleared when released. No automatic redirects to another host, proxy use,
 * model fallback, transport retry, or live request during connect. Tools use
 * complete validated argument objects; continuation fields are retained and
 * round-tripped under namespace openrouter.chat-completions, format json-v1.
 * That format is UTF-8 strict JSON object {"reasoning_details":[...]} containing
 * only the optional reasoning_details assistant wire member; explicit presence
 * of an empty array is retained/replayed and is never collapsed to absence; array entries are
 * reconstructed losslessly by the bounded rules below, never interpreted/rendered.
 * No other top-level key is accepted in v1; unknown required continuation fields
 * fail UNSUPPORTED before a tool executes or continuation sends. The codec
 * accumulates reasoning_details in wire order. Consecutive reasoning.text
 * entries concatenate only their text strings until a signature closes that
 * block; consecutive reasoning.summary entries concatenate summary strings.
 * Known non-payload members (id/index/format) must be absent or agree within a
 * merged block; a newly supplied signature is retained exactly, never joined
 * to another signature. reasoning.encrypted entries are discrete opaque data
 * objects, never concatenated or merged. Unknown types/ambiguous conflicting
 * fields fail UNSUPPORTED/PROTOCOL before tool admission; never guess from index
 * alone. Unknown members in discrete objects are preserved. For a merged
 * text/summary block, preserve the union of all non-payload members; repeated
 * members must have equal JSON values or fail UNSUPPORTED, never discard an
 * unfamiliar member or concatenate it speculatively. Absence does not erase a
 * previously supplied member. One entry with a signature closes a text block.
 * A nonstream response already contains the final array. The bounded private
 * writer generates one final array after reconstruction;
 * send reinserts it beside role/content/tool_calls, never inside visible text.
 * Tool ID/name wire strings may be fragmented and interleaved with argument
 * fragments. The adapter assembles all identity fragments privately until the
 * validated wire turn ends; then emits exactly one TOOL_BEGIN per index with
 * complete ID/name, followed by retained/coalesced ARGUMENTS and TOOL_END. It
 * never guesses identity completion from the first argument fragment. No
 * tool event is executable before the successful FINISH. Text may stream earlier.
 * Duplicate keys, malformed arrays or invalid UTF-8 fail PROTOCOL. Opaque data
 * remains within limits.max_opaque_bytes and persists exact generated bytes.
 */
extern const cog_provider_vtable_t cog_openrouter_provider_vtable;
#ifdef __cplusplus
}
#endif
#endif /* COG_OPENROUTER_H */
