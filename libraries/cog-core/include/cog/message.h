/**
 * @file message.h
 * @brief Provider-neutral complete messages, tool identity and continuation.
 *
 * All pointers are borrowed immutable views. send() copies their transitive
 * contents before returning; session insertion deep-copies them into the
 * session constructor's bounded arena. No function may retain caller storage
 * without an explicit copy. Text is UTF-8 without embedded NUL; byte lengths
 * exclude terminators. Opaque bytes may contain NUL and are never display text.
 * Threading: immutable views may be shared; owners serialize mutation/free.
 */
#ifndef COG_MESSAGE_H
#define COG_MESSAGE_H

#include <cog/result.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Role of one durable conversation message. */
typedef enum cog_message_role {
    COG_MESSAGE_ROLE_SYSTEM = 0,
    COG_MESSAGE_ROLE_USER,
    COG_MESSAGE_ROLE_ASSISTANT,
    COG_MESSAGE_ROLE_TOOL /**< Result correlated by tool_call_id, never name. */
} cog_message_role_t;

/** @brief Whether an assistant turn is safe to replay as completed history. */
typedef enum cog_message_status {
    COG_MESSAGE_COMPLETE = 0,
    COG_MESSAGE_INCOMPLETE,
    COG_MESSAGE_CANCELLED,
    COG_MESSAGE_FAILED
} cog_message_status_t;

/** @brief A complete identified tool invocation within one assistant message. */
typedef struct cog_message_tool_call {
    const char *id;       /**< Nonempty NUL-terminated provider call ID. */
    const char *name;     /**< Nonempty NUL-terminated table tool name. */
    const char *arguments; /**< Strict JSON object, complete before execution. */
    size_t arguments_len; /**< Bytes excluding NUL. */
} cog_message_tool_call_t;

/**
 * @brief Lossless provider-required continuation, separate from visible text.
 *
 * namespace_id identifies the provider protocol; format identifies its pinned
 * encoding/version (for example openrouter.chat-completions/json-v1). Bytes are
 * validated private protocol data, copied exactly and persisted, never logged
 * or rendered. A provider unable to replay this namespace/format fails with
 * COG_ERROR_UNSUPPORTED before sending. No reasoning generation is requested
 * merely to populate this field. All strings must be nonempty.
 */
typedef struct cog_message_opaque {
    const char *namespace_id;
    const char *format;
    const unsigned char *data;
    size_t size;
} cog_message_opaque_t;

/** @brief Reported usage; false availability means unknown, never zero billed. */
typedef struct cog_usage {
    bool tokens_known;       /**< Both input/output counts were reported. */
    uint64_t input_tokens;
    uint64_t output_tokens;
    bool reasoning_tokens_known;
    uint64_t reasoning_tokens;
    bool cost_known;         /**< Provider-reported cost, not a local estimate. */
    double cost;            /**< Nonnegative amount in currency when known. */
    const char *currency;   /**< ISO currency code; nonempty if cost_known. */
} cog_usage_t;

/**
 * @brief One complete message, with at most one assistant message per turn.
 *
 * Zero-initialize. Pointer/count pairs permit NULL iff count/length is zero.
 * content may accompany assistant tool_calls. Tool call IDs are unique within
 * an assistant turn; (assistant node ID, call ID) is the session execution key.
 * A TOOL result has nonempty tool_call_id and no tool_calls or opaque data;
 * name is descriptive only. Incomplete messages are retained for diagnostics,
 * excluded from ordinary replay, and require explicit application continuation.
 * The session validates associations on the selected branch; forked ancestors
 * are immutable and do not authorize re-executing their tools.
 */
typedef struct cog_message {
    cog_message_role_t role;
    const char *content;
    size_t content_len;
    const char *name;
    const cog_message_tool_call_t *tool_calls;
    size_t tool_call_count;
    const char *tool_call_id;
    const cog_message_opaque_t *opaque;
    size_t opaque_count;
    cog_message_status_t status;
    cog_usage_t usage;
} cog_message_t;

/**
 * @brief Validate message structure without side effects or JSON parsing.
 * @param[in] message Immutable message to validate.
 * @return COG_OK; NULL_PTR for NULL; INVALID_ARG for role, pointer/length,
 * UTF-8, duplicate/empty IDs or invalid usage. JSON syntax/schema and opaque
 * namespace/format support are validated by the consuming provider/toolkit.
 * @note Structural validation does not resolve branch result associations or
 * tool schemas. No allocation; not signal-safe; concurrent immutable reads OK.
 */
cog_result_t cog_message_validate(const cog_message_t *message);

#ifdef __cplusplus
}
#endif
#endif /* COG_MESSAGE_H */
