/**
 * @file message.h
 * @brief Chat message types used in provider requests.
 *
 * A message is a single turn in a conversation: a role, optional name,
 * and a content payload.  Messages are referenced, not owned, by a
 * @c cog_provider_request_t.
 *
 * Threading model: message structs are read-only after construction and
 * may be shared across threads as long as the underlying content strings
 * are not modified.  Individual provider implementations must not retain
 * message pointers beyond the call that receives them unless they copy
 * the data.
 */
#ifndef COG_MESSAGE_H
#define COG_MESSAGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Conversation role for a message.
 */
typedef enum cog_message_role {
    COG_MESSAGE_ROLE_SYSTEM = 0,    /**< System instruction. */
    COG_MESSAGE_ROLE_USER,          /**< User input. */
    COG_MESSAGE_ROLE_ASSISTANT,     /**< Model output. */
    COG_MESSAGE_ROLE_TOOL,          /**< Tool result; use @p name to identify
                                         the tool call. */
} cog_message_role_t;

/**
 * @brief One chat message.
 *
 * The @p content pointer is referenced, not owned.  The caller must
 * ensure the content outlives the request that contains it.
 */
typedef struct cog_message {
    cog_message_role_t role; /**< Role of this message. */
    const char *content;     /**< Message payload; may be NULL when
                                  @p content_len is zero. */
    size_t content_len;      /**< Length of @p content in bytes. */
    const char *name;        /**< Tool name for @c COG_MESSAGE_ROLE_TOOL;
                                  may be NULL otherwise. */
} cog_message_t;

#ifdef __cplusplus
}
#endif

#endif /* COG_MESSAGE_H */
