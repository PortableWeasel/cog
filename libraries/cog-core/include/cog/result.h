/**
 * @file result.h
 * @brief Result codes shared across cog-core.
 *
 * Every public function in cog-core returns a @c cog_result_t.  Zero
 * means success; negative values mean failure.  Error codes are stable
 * and extensible: new codes are appended at the end of the enumeration
 * without renumbering existing values.
 *
 * Threading model: these constants are pure values; no threading
 * constraints apply.
 */
#ifndef COG_RESULT_H
#define COG_RESULT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result code returned by cog-core public functions.
 */
typedef enum cog_result {
    COG_OK = 0,                     /**< Success. */
    COG_ERROR_INVALID_ARG = -1,     /**< A required argument was NULL, out of range,
                                         or malformed. */
    COG_ERROR_NULL_PTR = -2,        /**< A required pointer argument was NULL. */
    COG_ERROR_NOMEM = -3,           /**< Memory allocation failed. */
    COG_ERROR_NOT_FOUND = -4,       /**< The requested item does not exist. */
    COG_ERROR_OVERFLOW = -5,        /**< A buffer or counter would overflow. */
    COG_ERROR_UNHANDLED = -6,       /**< No handler is registered for the current
                                         (state, event) pair. */
    COG_ERROR_TRANSITION_DENIED = -7, /**< The requested state transition is not
                                         listed as allowed for the current entry. */
    COG_ERROR_AGAIN = -8,           /**< The operation could not complete now; the
                                         caller may retry later. */
    COG_ERROR_INVALID_STATE = -9,   /**< The object is in the wrong state for this
                                         call. */
    COG_ERROR_IO = -10,             /**< An input/output operation failed. */
    COG_ERROR_ABORTED = -11,        /**< The operation was cancelled or the run loop
                                         was stopped. */
} cog_result_t;

#ifdef __cplusplus
}
#endif

#endif /* COG_RESULT_H */
