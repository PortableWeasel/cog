/**
 * @file handler.h
 * @brief Handler result contract and handler function type.
 *
 * A handler is a pure function that receives the core instance and the
 * event that triggered it, performs the work associated with the current
 * state, and returns the state it would like to transition to.
 *
 * Handlers are wired into static const dispatch tables (see @c state.h);
 * there is no dynamic registration.  Each handler must be re-entrant and
 * must not retain global mutable state between calls.
 *
 * Threading model: handlers are invoked on the same thread that runs
 * @c cog_core_run.  They must not block indefinitely; long-running work
 * should be delegated to an auxiliary process or returned as an
 * asynchronous event.
 */
#ifndef COG_HANDLER_H
#define COG_HANDLER_H

#include <cog/event.h>
#include <cog/result.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations to break the core/handler include cycle. */
typedef struct cog_core cog_core_t;

/**
 * @brief Result returned by a state-machine handler.
 *
 * The @c transition field tells the dispatcher which state to enter
 * next.  The @c status field tells the dispatcher whether the handler
 * succeeded.  A non-COG_OK status prevents the transition from being
 * applied and is reported by @c cog_core_run.
 */
typedef struct cog_handler_result {
    cog_state_t transition; /**< Target state id or @c COG_TRANSITION_SELF. */
    cog_result_t status;    /**< Handler success/failure. */
} cog_handler_result_t;

/**
 * @brief Handler function signature.
 *
 * @param[in,out] core  Core instance that owns the state machine.
 * @param[in]     event Event that triggered this handler.
 * @return A @c cog_handler_result_t describing the requested transition
 *         and handler status.
 *
 * @pre @p core is a fully initialised core instance.
 * @pre @p event is non-NULL and valid for the duration of the call.
 *
 * @post The returned transition is either @c COG_TRANSITION_SELF or an
 *       id that appears in the current dispatch entry's allowed list.
 */
typedef cog_handler_result_t (*cog_handler_fn)(cog_core_t *core,
                                                const cog_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* COG_HANDLER_H */
