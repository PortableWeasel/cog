/**
 * @file test_core_provider.c
 * @brief Cross-module integration test for core.c + provider.c.
 *
 * Configures a real cog-core agent with a fake static-vtable provider and
 * drives the full user-input -> provider-send -> LLM_CHUNK -> LLM_COMPLETE
 * flow through the real dispatch table and session history.  Covers:
 *
 *   - streaming: the provider yields several chunks; each is appended to
 *     the session tree as an assistant message and the final chunk closes
 *     the turn with LLM_COMPLETE;
 *   - error/retry metadata: a receive failure carries a cog_provider_error_t
 *     (http_status, retryable, retry_after_ms, message) that the handler
 *     reads off the provider and propagates out of cog_core_run;
 *   - cancellation: a receive returning COG_ERROR_ABORTED is handled
 *     gracefully (the turn returns to IDLE without appending a partial
 *     response);
 *   - provider accessor/ownership: cog_core_provider returns the configured
 *     instance and the core borrows it (cog_core_free does not disconnect
 *     it; the caller still owns it).
 *
 * The fake provider embeds the same first-member vtable layout as the
 * private struct cog_provider (see provider_internal.h) so the public
 * cog_provider_* wrappers dispatch correctly, but the test never includes
 * the private header: it stays a black-box consumer of cog-provider.
 *
 * Ticket: 1433731 (Stage 12.4e — Integrate provider and toolkit with core)
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/core.h"
#include "cog/provider.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test application vocabulary.                                        */
/* ------------------------------------------------------------------ */

enum {
    ST_IDLE = 0,
    ST_RUNNING = 1,
};

enum {
    EV_USER_INPUT = COG_EVENT_USER_INPUT,
    EV_LLM_CHUNK = COG_EVENT_LLM_CHUNK,
    EV_LLM_COMPLETE = COG_EVENT_LLM_COMPLETE,
};

/* ------------------------------------------------------------------ */
/* Fake static-vtable provider                                         */
/* ------------------------------------------------------------------ */

typedef enum fake_mode {
    FAKE_STREAM = 0,   /* several chunks then complete */
    FAKE_RECV_ERROR,   /* receive returns an error with retry metadata */
    FAKE_CANCEL,       /* receive returns COG_ERROR_ABORTED */
} fake_mode_t;

typedef struct fake_script {
    fake_mode_t mode;
    const char *chunks[8];
    int chunk_count;
    cog_result_t recv_result;
    int http_status;
    bool retryable;
    uint32_t retry_after_ms;
    const char *error_message;
} fake_script_t;

/* The fake provider layout: the first member must be the static vtable so
 * the public cog_provider_* wrappers can dispatch through the opaque
 * pointer (mirrors struct cog_provider in provider_internal.h). */
typedef struct fake_provider {
    const cog_provider_vtable_t *vtable;
    const fake_script_t *script;
    int chunk_index;
    bool sent;
    bool disconnected;
    const cog_provider_request_t *last_request;
    cog_provider_error_t last_error;
} fake_provider_t;

/* The script for the next fake_connect.  Single-threaded test; the
 * endpoint's api_key is unused so the pointer avoids a cast-align
 * warning. */
static const fake_script_t *g_current_script = NULL;

static cog_result_t fake_connect(const cog_endpoint_t *endpoint,
                                 cog_provider_t **out_provider);
static cog_result_t fake_disconnect(cog_provider_t *provider);
static cog_result_t fake_send(cog_provider_t *provider,
                              const cog_provider_request_t *request);
static cog_result_t fake_receive(cog_provider_t *provider,
                                 cog_provider_response_t *response);

static const cog_provider_vtable_t fake_vtable = {
    fake_connect,
    fake_disconnect,
    fake_send,
    fake_receive,
};

static cog_result_t fake_connect(const cog_endpoint_t *endpoint,
                                 cog_provider_t **out_provider)
{
    fake_provider_t *p;

    if (endpoint == NULL || out_provider == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    p = (fake_provider_t *)calloc(1U, sizeof(*p));
    if (p == NULL) {
        return COG_ERROR_NOMEM;
    }
    p->vtable = &fake_vtable;
    p->script = g_current_script;
    *out_provider = (cog_provider_t *)p;
    return COG_OK;
}

static cog_result_t fake_disconnect(cog_provider_t *provider)
{
    fake_provider_t *p = (fake_provider_t *)provider;

    if (p == NULL) {
        return COG_OK;
    }
    p->disconnected = true;
    free(p);
    return COG_OK;
}

static cog_result_t fake_send(cog_provider_t *provider,
                              const cog_provider_request_t *request)
{
    fake_provider_t *p = (fake_provider_t *)provider;

    if (p == NULL || request == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (p->sent) {
        return COG_ERROR_INVALID_STATE;
    }
    p->last_request = request;
    p->sent = true;
    return COG_OK;
}

static cog_result_t fake_receive(cog_provider_t *provider,
                                 cog_provider_response_t *response)
{
    fake_provider_t *p = (fake_provider_t *)provider;
    const fake_script_t *s;
    const char *chunk;
    size_t len;

    if (p == NULL || response == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!p->sent) {
        return COG_ERROR_INVALID_STATE;
    }
    s = p->script;
    if (s == NULL) {
        return COG_ERROR_INVALID_STATE;
    }

    switch (s->mode) {
    case FAKE_RECV_ERROR:
        p->last_error.code = s->recv_result;
        p->last_error.http_status = s->http_status;
        p->last_error.message = s->error_message;
        p->last_error.retryable = s->retryable;
        p->last_error.retry_after_ms = s->retry_after_ms;
        return s->recv_result;
    case FAKE_CANCEL:
        return COG_ERROR_ABORTED;
    case FAKE_STREAM:
    default:
        break;
    }

    chunk = (p->chunk_index < s->chunk_count) ? s->chunks[p->chunk_index]
                                              : NULL;
    if (chunk == NULL) {
        if (response->content_capacity > 0U) {
            response->content[0] = '\0';
        }
        response->content_len = 0U;
        response->finish_reason = COG_FINISH_REASON_STOP;
        response->is_complete = true;
        return COG_OK;
    }
    len = strlen(chunk);
    if (len >= response->content_capacity) {
        return COG_ERROR_OVERFLOW;
    }
    (void)memcpy(response->content, chunk, len);
    response->content[len] = '\0';
    response->content_len = len;
    p->chunk_index++;
    if (p->chunk_index >= s->chunk_count) {
        response->finish_reason = COG_FINISH_REASON_STOP;
        response->is_complete = true;
    } else {
        response->finish_reason = COG_FINISH_REASON_NONE;
        response->is_complete = false;
    }
    return COG_OK;
}

static const cog_endpoint_t fake_endpoint = {
    .name = "fake",
    .base_url = NULL,
    .api_version = NULL,
    .api_key = NULL,
    .vtable = &fake_vtable,
    .models = NULL,
};

/* ------------------------------------------------------------------ */
/* Handler-side capture (test-owned; not part of any library).         */
/* ------------------------------------------------------------------ */

typedef struct provider_capture {
    cog_result_t last_status;
    int http_status;
    bool retryable;
    uint32_t retry_after_ms;
    const char *message;
} provider_capture_t;

static provider_capture_t g_capture;

/* ------------------------------------------------------------------ */
/* Handlers                                                            */
/* ------------------------------------------------------------------ */

/** ST_IDLE + USER_INPUT: append the user message, send to the provider,
 * and enqueue the first LLM_CHUNK to start streaming. */
static cog_handler_result_t on_user_input(cog_core_t *core,
                                          const cog_event_t *event)
{
    cog_provider_t *provider = cog_core_provider(core);
    cog_provider_request_t req;
    cog_message_t msg;
    cog_event_t chunk_ev;
    cog_handler_result_t r = {ST_RUNNING, COG_OK};

    if (core == NULL || event == NULL) {
        r.status = COG_ERROR_NULL_PTR;
        return r;
    }
    msg.role = COG_MESSAGE_ROLE_USER;
    msg.content = (const char *)event->payload;
    msg.content_len = event->payload_len;
    msg.name = NULL;
    if (cog_session_append(cog_core_session(core), &msg, NULL) != COG_OK) {
        r.status = COG_ERROR_IO;
        return r;
    }
    memset(&req, 0, sizeof(req));
    req.model_id = "fake-model";
    req.messages = &msg;
    req.message_count = 1U;
    req.stream = true;
    if (cog_provider_send(provider, &req) != COG_OK) {
        r.status = COG_ERROR_IO;
        return r;
    }
    chunk_ev.id = (cog_event_id_t)EV_LLM_CHUNK;
    chunk_ev.kind = COG_EVENT_LLM_CHUNK;
    chunk_ev.payload = NULL;
    chunk_ev.payload_len = 0U;
    chunk_ev.source_id = 0U;
    if (cog_core_enqueue(core, &chunk_ev) != COG_OK) {
        r.status = COG_ERROR_IO;
        return r;
    }
    return r;
}

/** ST_RUNNING + LLM_CHUNK: receive the next chunk, append it to the
 * session, and enqueue the next chunk or LLM_COMPLETE.  A receive error
 * propagates its status (recording retry metadata); a cancellation returns
 * the turn to IDLE without appending a partial response. */
static cog_handler_result_t on_llm_chunk(cog_core_t *core,
                                         const cog_event_t *event)
{
    cog_provider_t *provider = cog_core_provider(core);
    cog_provider_response_t resp;
    char content[256];
    cog_message_t msg;
    cog_event_t next;
    cog_handler_result_t r = {COG_TRANSITION_SELF, COG_OK};
    cog_result_t rc;

    if (core == NULL || event == NULL) {
        r.status = COG_ERROR_NULL_PTR;
        return r;
    }
    memset(&resp, 0, sizeof(resp));
    resp.content = content;
    resp.content_capacity = sizeof(content);

    rc = cog_provider_receive(provider, &resp);
    if (rc != COG_OK) {
        if (rc == COG_ERROR_ABORTED) {
            /* Cooperative cancellation: drop the partial turn. */
            r.transition = ST_IDLE;
            return r;
        }
        {
            fake_provider_t *fp = (fake_provider_t *)provider;
            g_capture.last_status = rc;
            g_capture.http_status = fp->last_error.http_status;
            g_capture.retryable = fp->last_error.retryable;
            g_capture.retry_after_ms = fp->last_error.retry_after_ms;
            g_capture.message = fp->last_error.message;
        }
        r.status = rc;
        return r;
    }

    msg.role = COG_MESSAGE_ROLE_ASSISTANT;
    msg.content = content;
    msg.content_len = resp.content_len;
    msg.name = NULL;
    if (cog_session_append(cog_core_session(core), &msg, NULL) != COG_OK) {
        r.status = COG_ERROR_IO;
        return r;
    }

    next.id = (cog_event_id_t)(resp.is_complete ? EV_LLM_COMPLETE
                                                : EV_LLM_CHUNK);
    next.kind = resp.is_complete ? COG_EVENT_LLM_COMPLETE
                                 : COG_EVENT_LLM_CHUNK;
    next.payload = NULL;
    next.payload_len = 0U;
    next.source_id = 0U;
    if (cog_core_enqueue(core, &next) != COG_OK) {
        r.status = COG_ERROR_IO;
        return r;
    }
    return r;
}

/** ST_RUNNING + LLM_COMPLETE: the turn is finished; return to IDLE. */
static cog_handler_result_t on_llm_complete(cog_core_t *core,
                                            const cog_event_t *event)
{
    cog_handler_result_t r = {ST_IDLE, COG_OK};

    if (core == NULL || event == NULL) {
        r.status = COG_ERROR_NULL_PTR;
        return r;
    }
    return r;
}

static const cog_state_t idle_transitions[] = {
    ST_RUNNING,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_state_t running_transitions[] = {
    ST_IDLE,
    COG_TRANSITION_SELF,
    COG_TRANSITION_END,
};

static const cog_dispatch_entry_t dispatch_table[] = {
    COG_DISPATCH_ENTRY(ST_IDLE, EV_USER_INPUT, on_user_input, idle_transitions),
    COG_DISPATCH_ENTRY(ST_RUNNING, EV_LLM_CHUNK, on_llm_chunk,
                       running_transitions),
    COG_DISPATCH_ENTRY(ST_RUNNING, EV_LLM_COMPLETE, on_llm_complete,
                       running_transitions),
};

/* ------------------------------------------------------------------ */
/* Wait hook: stop the loop on the first wait.                         */
/* ------------------------------------------------------------------ */

typedef struct wait_ctx {
    cog_core_t *core;
    int calls;
} wait_ctx_t;

static void stop_wait_fn(void *arg)
{
    wait_ctx_t *ctx = (wait_ctx_t *)arg;

    ctx->calls++;
    cog_core_stop(ctx->core);
}

/* ------------------------------------------------------------------ */
/* Assertion helpers.                                                  */
/* ------------------------------------------------------------------ */

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                    #cond);                                             \
            ++failures;                                                 \
        }                                                               \
    } while (0)

#define CHECK_EQ_INT(a, b)                                              \
    do {                                                                \
        long long _a = (long long)(a);                                  \
        long long _b = (long long)(b);                                  \
        if (_a != _b) {                                                 \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n",    \
                    __FILE__, __LINE__, #a, #b, _a, _b);                \
            ++failures;                                                 \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */
/* Streaming flow + accessor/ownership                                 */
/* ------------------------------------------------------------------ */

static void test_streaming_flow(void)
{
    static const fake_script_t stream_script = {
        .mode = FAKE_STREAM,
        .chunks = { "Hello", " world", "!" },
        .chunk_count = 3,
    };
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    cog_provider_t *provider = NULL;
    fake_provider_t *fp;
    wait_ctx_t ctx;
    cog_event_t ev;
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

    g_current_script = &stream_script;
    CHECK(cog_provider_connect(&fake_endpoint, &provider) == COG_OK);
    CHECK(provider != NULL);
    fp = (fake_provider_t *)provider;

    CHECK(cog_session_new(&session, NULL) == COG_OK);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.provider = provider;
    cfg.wait_fn = stop_wait_fn;
    cfg.wait_arg = &ctx;

    CHECK(cog_core_new(&core, &cfg) == COG_OK);
    CHECK(cog_core_provider(core) == provider);

    memset(&ctx, 0, sizeof(ctx));
    ctx.core = core;

    memset(&ev, 0, sizeof(ev));
    ev.id = (cog_event_id_t)EV_USER_INPUT;
    ev.kind = COG_EVENT_USER_INPUT;
    ev.payload = "hello";
    ev.payload_len = 5U;
    CHECK(cog_core_enqueue(core, &ev) == COG_OK);

    CHECK(cog_core_run(core) == COG_OK);

    /* Session history: user + three assistant chunks. */
    CHECK(cog_session_get_history(session, cog_session_active(session),
                                  &msgs, &count) == COG_OK);
    CHECK_EQ_INT(count, 4);
    CHECK(msgs[0].role == COG_MESSAGE_ROLE_USER);
    CHECK(msgs[0].content_len == 5U);
    CHECK(memcmp(msgs[0].content, "hello", 5U) == 0);
    CHECK(msgs[1].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(msgs[1].content_len == 5U);
    CHECK(memcmp(msgs[1].content, "Hello", 5U) == 0);
    CHECK(msgs[2].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(msgs[2].content_len == 6U);
    CHECK(memcmp(msgs[2].content, " world", 6U) == 0);
    CHECK(msgs[3].role == COG_MESSAGE_ROLE_ASSISTANT);
    CHECK(msgs[3].content_len == 1U);
    CHECK(memcmp(msgs[3].content, "!", 1U) == 0);

    /* The provider was sent exactly one request. */
    CHECK(fp->sent);

    /* Ownership: the core borrows the provider; cog_core_free must not
     * disconnect it.  Capture the flag before disconnect frees the
     * instance. */
    cog_core_free(core);
    CHECK(!fp->disconnected);
    {
        bool was_disconnected = fp->disconnected;
        CHECK(cog_provider_disconnect(provider) == COG_OK);
        CHECK(!was_disconnected);
    }

    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* Error / retry metadata propagation                                  */
/* ------------------------------------------------------------------ */

static void test_error_retry_metadata(void)
{
    static const fake_script_t error_script = {
        .mode = FAKE_RECV_ERROR,
        .recv_result = COG_ERROR_UNAVAILABLE,
        .http_status = 429,
        .retryable = true,
        .retry_after_ms = 5000U,
        .error_message = "rate limited",
    };
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    cog_provider_t *provider = NULL;
    wait_ctx_t ctx;
    cog_event_t ev;

    memset(&g_capture, 0, sizeof(g_capture));
    g_current_script = &error_script;
    CHECK(cog_provider_connect(&fake_endpoint, &provider) == COG_OK);

    CHECK(cog_session_new(&session, NULL) == COG_OK);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.provider = provider;
    cfg.wait_fn = stop_wait_fn;
    cfg.wait_arg = &ctx;

    CHECK(cog_core_new(&core, &cfg) == COG_OK);

    memset(&ctx, 0, sizeof(ctx));
    ctx.core = core;

    memset(&ev, 0, sizeof(ev));
    ev.id = (cog_event_id_t)EV_USER_INPUT;
    ev.kind = COG_EVENT_USER_INPUT;
    ev.payload = "hi";
    ev.payload_len = 2U;
    CHECK(cog_core_enqueue(core, &ev) == COG_OK);

    /* The receive failure propagates out of the run loop. */
    CHECK(cog_core_run(core) == COG_ERROR_UNAVAILABLE);

    /* The handler read the retry metadata off the provider. */
    CHECK(g_capture.last_status == COG_ERROR_UNAVAILABLE);
    CHECK_EQ_INT(g_capture.http_status, 429);
    CHECK(g_capture.retryable);
    CHECK_EQ_INT((long long)g_capture.retry_after_ms, 5000LL);
    CHECK(g_capture.message != NULL);
    CHECK(strcmp(g_capture.message, "rate limited") == 0);

    cog_core_free(core);
    CHECK(cog_provider_disconnect(provider) == COG_OK);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* Cancellation                                                        */
/* ------------------------------------------------------------------ */

static void test_cancellation(void)
{
    static const fake_script_t cancel_script = {
        .mode = FAKE_CANCEL,
    };
    cog_session_t *session = NULL;
    cog_core_t *core = NULL;
    cog_core_config_t cfg;
    cog_provider_t *provider = NULL;
    wait_ctx_t ctx;
    cog_event_t ev;
    const cog_message_t *msgs = NULL;
    size_t count = 0U;

    g_current_script = &cancel_script;
    CHECK(cog_provider_connect(&fake_endpoint, &provider) == COG_OK);

    CHECK(cog_session_new(&session, NULL) == COG_OK);

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = session;
    cfg.dispatch_table = dispatch_table;
    cfg.dispatch_table_size = sizeof(dispatch_table) / sizeof(dispatch_table[0]);
    cfg.provider = provider;
    cfg.wait_fn = stop_wait_fn;
    cfg.wait_arg = &ctx;

    CHECK(cog_core_new(&core, &cfg) == COG_OK);

    memset(&ctx, 0, sizeof(ctx));
    ctx.core = core;

    memset(&ev, 0, sizeof(ev));
    ev.id = (cog_event_id_t)EV_USER_INPUT;
    ev.kind = COG_EVENT_USER_INPUT;
    ev.payload = "cancel me";
    ev.payload_len = 9U;
    CHECK(cog_core_enqueue(core, &ev) == COG_OK);

    /* Cancellation is handled gracefully: the loop returns COG_OK and the
     * turn returns to IDLE. */
    CHECK(cog_core_run(core) == COG_OK);
    CHECK(cog_core_state(core) == ST_IDLE);

    /* No partial assistant response was appended; only the user turn. */
    CHECK(cog_session_get_history(session, cog_session_active(session),
                                  &msgs, &count) == COG_OK);
    CHECK_EQ_INT(count, 1);
    CHECK(msgs[0].role == COG_MESSAGE_ROLE_USER);

    cog_core_free(core);
    CHECK(cog_provider_disconnect(provider) == COG_OK);
    cog_session_free(session);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    test_streaming_flow();
    test_error_retry_metadata();
    test_cancellation();

    if (failures != 0) {
        fprintf(stderr, "test_core_provider: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_core_provider: all checks passed\n");
    return 0;
}
