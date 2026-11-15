/**
 * @file mock_provider.c
 * @brief Deterministic scripted mock LLM provider (see mock_provider.h).
 *
 * The mock provider is application/test scaffolding, kept out of
 * cog-provider on purpose.  It implements the full static vtable contract
 * (connect / disconnect / send / receive) with no network access.
 *
 * The instance layout mirrors the private @c struct cog_provider from
 * cog-provider's provider_internal.h: the first member is the static
 * vtable pointer, so the public @c cog_provider_* convenience wrappers
 * dispatch correctly.  This file never includes the private header; it
 * stays a black-box consumer of cog-provider, exactly like the fake
 * provider in tests/integration/test_core_provider.c.
 */
#define _POSIX_C_SOURCE 200809L

#include "cog_cli/mock_provider.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** Default script used when no script is supplied. */
static const cog_mock_step_t mock_default_steps[] = {
    {COG_MOCK_STEP_CHUNK, "mock response", COG_OK, 0, false, 0U, NULL},
};

/**
 * @brief Concrete layout of the opaque mock provider instance.
 *
 * The vtable pointer must remain the first member (see file header).
 */
typedef struct cog_mock_provider {
    const cog_provider_vtable_t *vtable; /**< Static vtable. */
    cog_mock_script_t script;            /**< Owned copy of the script. */
    size_t step_index;                   /**< Next step to consume. */
    size_t passes_left;                  /**< Replays remaining. */
    bool cancel_requested;               /**< One-shot cancel armed. */
    bool sent;                           /**< A request is in flight. */
    bool disconnected;                   /**< Disconnect called. */
    size_t send_count;                   /**< Successful sends. */
    size_t receive_count;                /**< Receive calls. */
    size_t chunk_count;                  /**< Chunks emitted. */
    size_t error_count;                  /**< Error steps delivered. */
    size_t cancel_count;                 /**< ABORTED receives. */
    cog_provider_request_t last_request; /**< Copy of the most recent
                                              request struct (its pointer
                                              fields remain borrowed). */
    cog_provider_error_t last_error;     /**< Last scripted error. */
} cog_mock_provider_t;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/** Sleep for @p ms milliseconds (bounded, interruptible by signals). */
static void mock_sleep_ms(uint32_t ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000U);
    ts.tv_nsec = (long)((ms % 1000U) * 1000000UL);
    (void)nanosleep(&ts, NULL);
}

/** Fill a response with an empty STOP completion. */
static void mock_fill_complete(cog_provider_response_t *response)
{
    if (response->content_capacity > 0U) {
        response->content[0] = '\0';
    }
    response->content_len = 0U;
    response->finish_reason = COG_FINISH_REASON_STOP;
    response->is_complete = true;
}

/** Validate a script's borrowed pointers. */
static cog_result_t mock_validate_script(const cog_mock_script_t *script)
{
    if (script == NULL) {
        return COG_OK;
    }
    if (script->steps == NULL && script->step_count > 0U) {
        return COG_ERROR_INVALID_ARG;
    }
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Vtable implementation                                               */
/* ------------------------------------------------------------------ */

static cog_result_t mock_connect(const cog_endpoint_t *endpoint,
                                 cog_provider_t **out_provider)
{
    const cog_mock_script_t *script = NULL;

    if (endpoint == NULL || out_provider == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (endpoint->api_key != NULL) {
        /* The endpoint's api_key carries the script bytes.  Copy by value
         * (alignment-safe) so the provider is self-contained. */
        script = (const cog_mock_script_t *)(const void *)endpoint->api_key;
    }
    return cog_mock_provider_new(script, out_provider);
}

static cog_result_t mock_disconnect(cog_provider_t *provider)
{
    cog_mock_provider_t *p = (cog_mock_provider_t *)provider;

    if (p == NULL) {
        return COG_OK;
    }
    p->disconnected = true;
    free(p);
    return COG_OK;
}

static cog_result_t mock_send(cog_provider_t *provider,
                              const cog_provider_request_t *request)
{
    cog_mock_provider_t *p = (cog_mock_provider_t *)provider;

    if (p == NULL || request == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (p->disconnected) {
        return COG_ERROR_INVALID_STATE;
    }
    if (p->script.send_error != COG_OK) {
        return p->script.send_error;
    }

    p->last_request = *request;
    ++p->send_count;
    p->sent = true;
    p->cancel_requested = false;

    /* Start a fresh pass when the previous one is exhausted and replays
     * remain.  SIZE_MAX means "repeat forever". */
    if (p->step_index >= p->script.step_count && p->passes_left > 0U) {
        p->step_index = 0U;
        if (p->passes_left != SIZE_MAX) {
            --p->passes_left;
        }
    }
    return COG_OK;
}

static cog_result_t mock_receive(cog_provider_t *provider,
                                 cog_provider_response_t *response)
{
    cog_mock_provider_t *p = (cog_mock_provider_t *)provider;
    const cog_mock_step_t *step;
    size_t len;

    if (p == NULL || response == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!p->sent) {
        return COG_ERROR_INVALID_STATE;
    }
    ++p->receive_count;

    /* One-shot user-side cancel. */
    if (p->cancel_requested) {
        p->cancel_requested = false;
        ++p->cancel_count;
        return COG_ERROR_ABORTED;
    }

    /* Exhausted script: complete with empty content. */
    if (p->step_index >= p->script.step_count) {
        mock_fill_complete(response);
        return COG_OK;
    }

    step = &p->script.steps[p->step_index];
    ++p->step_index;

    switch (step->kind) {
    case COG_MOCK_STEP_CHUNK:
        if (p->script.chunk_delay_ms > 0U) {
            mock_sleep_ms(p->script.chunk_delay_ms);
        }
        len = strlen(step->chunk);
        if (len >= response->content_capacity) {
            return COG_ERROR_OVERFLOW;
        }
        (void)memcpy(response->content, step->chunk, len);
        response->content[len] = '\0';
        response->content_len = len;
        ++p->chunk_count;
        if (p->step_index >= p->script.step_count) {
            /* Last step of the pass: complete. */
            response->finish_reason = COG_FINISH_REASON_STOP;
            response->is_complete = true;
        } else {
            response->finish_reason = COG_FINISH_REASON_NONE;
            response->is_complete = false;
        }
        return COG_OK;

    case COG_MOCK_STEP_ERROR:
        p->last_error.code = step->error_code;
        p->last_error.http_status = step->http_status;
        p->last_error.message = step->error_message;
        p->last_error.retryable = step->retryable;
        p->last_error.retry_after_ms = step->retry_after_ms;
        ++p->error_count;
        return step->error_code;

    case COG_MOCK_STEP_CANCEL:
        ++p->cancel_count;
        return COG_ERROR_ABORTED;

    case COG_MOCK_STEP_COMPLETE:
        mock_fill_complete(response);
        return COG_OK;
    }
    return COG_ERROR_INVALID_STATE;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

const cog_provider_vtable_t cog_mock_provider_vtable = {
    mock_connect,
    mock_disconnect,
    mock_send,
    mock_receive,
};

const cog_endpoint_t cog_mock_endpoint = {
    .name = "mock",
    .base_url = NULL,
    .api_version = NULL,
    .api_key = NULL, /* default script */
    .vtable = &cog_mock_provider_vtable,
    .models = NULL,
};

cog_result_t cog_mock_provider_new(const cog_mock_script_t *script,
                                   cog_provider_t **out)
{
    cog_mock_provider_t *p;
    cog_result_t rc;

    if (out == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out = NULL;

    rc = mock_validate_script(script);
    if (rc != COG_OK) {
        return rc;
    }

    p = (cog_mock_provider_t *)calloc(1U, sizeof(*p));
    if (p == NULL) {
        return COG_ERROR_NOMEM;
    }
    p->vtable = &cog_mock_provider_vtable;
    if (script != NULL) {
        p->script = *script;
    } else {
        p->script.steps = mock_default_steps;
        p->script.step_count =
            sizeof(mock_default_steps) / sizeof(mock_default_steps[0]);
    }
    p->passes_left = p->script.repeat;

    *out = (cog_provider_t *)p;
    return COG_OK;
}

cog_result_t cog_mock_provider_connect(const cog_endpoint_t *endpoint,
                                       cog_provider_t **out)
{
    return mock_connect(endpoint, out);
}

cog_result_t cog_mock_provider_disconnect(cog_provider_t *provider)
{
    return mock_disconnect(provider);
}

cog_result_t cog_mock_provider_send(cog_provider_t *provider,
                                    const cog_provider_request_t *request)
{
    return mock_send(provider, request);
}

cog_result_t cog_mock_provider_receive(cog_provider_t *provider,
                                       cog_provider_response_t *response)
{
    return mock_receive(provider, response);
}

cog_result_t cog_mock_provider_request_cancel(cog_provider_t *provider)
{
    cog_mock_provider_t *p = (cog_mock_provider_t *)provider;

    if (p == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    p->cancel_requested = true;
    return COG_OK;
}

cog_result_t cog_mock_provider_state(cog_provider_t *provider,
                                     cog_mock_state_t *out)
{
    cog_mock_provider_t *p = (cog_mock_provider_t *)provider;

    if (p == NULL || out == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    out->send_count = p->send_count;
    out->receive_count = p->receive_count;
    out->chunk_count = p->chunk_count;
    out->error_count = p->error_count;
    out->cancel_count = p->cancel_count;
    out->disconnected = p->disconnected;
    out->cancel_requested = p->cancel_requested;
    out->step_index = p->step_index;
    out->last_request = &p->last_request;
    out->last_error = p->last_error;
    return COG_OK;
}
