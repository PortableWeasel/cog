/**
 * @file session.h
 * @brief Opaque conversation session context.
 *
 * A session holds the conversation state for a single agent loop:
 * message history, identity, and any context that must survive across
 * events and reconnections.  Internals are hidden; callers interact with
 * the session only through the functions declared here.
 *
 * Threading model: a session is owned by exactly one core instance and is
 * not thread-safe.  All access happens on the thread that runs
 * @c cog_core_run.
 */
#ifndef COG_SESSION_H
#define COG_SESSION_H

#include <cog/event.h>
#include <cog/message.h>
#include <cog/result.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque conversation session.
 *
 * Created with @c cog_session_new and destroyed with @c cog_session_free.
 */
typedef struct cog_session cog_session_t;

/**
 * @brief Configuration passed to @c cog_session_new.
 *
 * Callers should zero-initialise this struct to obtain defaults; adding
 * fields later will not break callers that zero-init.
 */
typedef struct cog_session_config {
    /**
     * @brief Optional session identifier.
     *
     * May be NULL; if NULL the implementation generates a unique id.
     * The string is copied during initialisation.
     */
    const char *id;

    /**
     * @brief Maximum number of conversation events retained as context.
     *
     * Zero selects a sensible default.
     */
    size_t max_context_events;
} cog_session_config_t;

/**
 * @brief Create a new session.
 *
 * @param[out] out   Uninitialised pointer; set to the new session on success.
 * @param[in]  config Configuration; may be NULL for defaults.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p out is NULL.
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * @post On success, @p *out is a valid session and the caller owns it.
 * @post On failure, @p *out is NULL and no resources are held.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_session_new(cog_session_t **out,
                             const cog_session_config_t *config);

/**
 * @brief Destroy a session.
 *
 * @param[in,out] session Session to destroy; may be NULL (no-op).
 *
 * @post All storage owned by @p session is released.  @p session must
 *       not be used after this call.
 *
 * Threading: not thread-safe.
 */
void cog_session_free(cog_session_t *session);

/**
 * @brief Return the session identifier.
 *
 * @param[in] session Session to query; may be NULL.
 * @return Pointer to a NUL-terminated id string, or NULL if @p session
 *         is NULL.
 *
 * @note The returned pointer remains valid until the session is freed or
 *       reset.
 *
 * Threading: not thread-safe.
 */
const char *cog_session_id(const cog_session_t *session);

/**
 * @brief Reset a session to its initial empty state.
 *
 * @param[in,out] session Session to reset.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p session is NULL.
 * @return COG_ERROR_INVALID_STATE if the session is currently active in a
 *         running core.
 *
 * @post The session retains its id but loses all accumulated context.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_session_reset(cog_session_t *session);

/**
 * @brief Return true if the session currently holds no context.
 *
 * @param[in] session Session to query; may be NULL.
 * @return true if the session is NULL or empty, false otherwise.
 *
 * Threading: not thread-safe.
 */
bool cog_session_is_empty(const cog_session_t *session);

/**
 * @brief Append a message to the session's conversation history.
 *
 * The message is copied into the session's internal storage.
 *
 * @param[in,out] session Session to modify.
 * @param[in]     msg     Message to append; must be non-NULL.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p session or @p msg is NULL.
 * @return COG_ERROR_NOMEM if allocation fails.
 * @return COG_ERROR_OVERFLOW if the session's history is full.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_session_add_message(cog_session_t *session,
                                      const cog_message_t *msg);

/**
 * @brief Get the conversation history for building a provider request.
 *
 * The returned array is owned by the session and remains valid until
 * the next call that modifies the session.
 *
 * @param[in]  session   Session to query.
 * @param[out] out_msgs  Receives pointer to the message array.
 * @param[out] out_count Receives the number of messages.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if any argument is NULL.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_session_get_history(const cog_session_t *session,
                                      const cog_message_t **out_msgs,
                                      size_t *out_count);

/**
 * @brief Return the parent session identifier.
 *
 * @param[in] session Session to query; may be NULL.
 * @return Parent session id, or NULL if this is a root session or
 *         @p session is NULL.
 *
 * Threading: not thread-safe.
 */
const char *cog_session_parent_id(const cog_session_t *session);

/**
 * @brief Set the parent session identifier.
 *
 * @param[in,out] session   Session to modify.
 * @param[in]     parent_id Parent session id; copied.  May be NULL to
 *                          clear the parent.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p session is NULL.
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_session_set_parent(cog_session_t *session,
                                     const char *parent_id);

#ifdef __cplusplus
}
#endif

#endif /* COG_SESSION_H */
