/**
 * @file test_provider.c
 * @brief Contract test for cog/provider.h, provider_request.h,
 *        provider_vtable.h and endpoint.h (libraries/cog-provider).
 *
 * Drives registry and generic dispatch with deterministic static vtables.
 * Actual adapter/HTTPS coverage lives in the OpenRouter-specific tests. Covers:
 *
 *   - request/response validation (NULL, malformed, boundary)
 *   - endpoint/model registry lookup, capability, and model selection
 *   - static vtable dispatch (connect/disconnect/send/receive wrappers)
 *   - single-turn and streaming responses
 *   - cancellation (COG_ERROR_ABORTED)
 *   - detailed provider errors with retryable / retry_after_ms
 *   - message history passed through to the provider
 *   - a real transport path through cog-net against a loopback server:
 *     streaming completion, 429 Retry-After metadata, malformed responses,
 *     overflow, and cancellation
 *   - ownership and NULL/not-found semantics
 *
 * Ticket: 80e64c5 (Stage 12.4a — Implement provider.c)
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/net.h"
#include "cog/provider.h"

#include "provider_internal.h"

#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,    \
                          #cond);                                            \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* Time helpers                                                         */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Scripted fake provider                                              */
/* ------------------------------------------------------------------ */

typedef enum fake_mode {
    FAKE_SINGLE = 0,   /* one complete response */
    FAKE_STREAM,       /* multiple chunks until complete */
    FAKE_SEND_ERROR,   /* send returns an error */
    FAKE_RECV_ERROR,   /* receive returns an error with retry metadata */
    FAKE_CANCEL,       /* receive returns COG_ERROR_ABORTED */
} fake_mode_t;

typedef struct fake_script {
    fake_mode_t mode;
    const char *chunks[8];
    int chunk_count;
    cog_result_t send_result;
    cog_result_t recv_result;
    int http_status;
    bool retryable;
    uint32_t retry_after_ms;
    const char *error_message;
} fake_script_t;

typedef struct fake_provider {
    struct cog_provider base;
    const fake_script_t *script;
    int chunk_index;
    bool sent;
    bool typed_finish;
    uint64_t typed_sequence;
    cog_result_t typed_outcome;
    bool disconnected;
    const cog_provider_request_t *last_request;
    cog_provider_error_t last_error;
} fake_provider_t;

/* The script to use for the next fake_connect.  Single-threaded test; the
 * endpoint's api_key is not used so the script pointer avoids a
 * cast-align warning. */
static const fake_script_t *g_current_script = NULL;

static cog_result_t fake_connect(const cog_endpoint_t *endpoint,
                                 cog_provider_t **out_provider);
static cog_result_t fake_disconnect(cog_provider_t *provider);
static cog_result_t fake_send(cog_provider_t *provider,
                              const cog_provider_request_t *request);
static cog_result_t fake_receive(cog_provider_t *provider,
                                 cog_provider_response_t *response);

/* The deterministic fake has no transport timer; ready steps need no poll. */
static cog_result_t fake_poll(cog_provider_t *provider, uint64_t now, uint64_t *deadline)
{
    (void)now;
    if (provider == NULL || deadline == NULL) return COG_ERROR_NULL_PTR;
    *deadline = UINT64_MAX;
    return COG_OK;
}
static cog_result_t fake_cancel(cog_provider_t *provider)
{
    fake_provider_t *p = (fake_provider_t *)provider;
    if (p == NULL) return COG_ERROR_NULL_PTR;
    if (p->sent) { p->typed_finish = true; p->typed_outcome = COG_ERROR_ABORTED; }
    return COG_OK;
}
static cog_result_t fake_next(cog_provider_t *provider, cog_provider_delta_t *out,
                             void *storage, size_t capacity, size_t *required)
{
    fake_provider_t *p = (fake_provider_t *)provider;
    if (p == NULL || out == NULL || required == NULL) return COG_ERROR_NULL_PTR;
    if (!p->sent) return COG_ERROR_INVALID_STATE;
    *required = 0U;
    if (p->typed_finish) {
        *out = (cog_provider_delta_t){.kind = COG_PROVIDER_DELTA_FINISH,
            .sequence = ++p->typed_sequence, .outcome = p->typed_outcome,
            .finish_reason = p->typed_outcome == COG_OK ? COG_FINISH_REASON_STOP : COG_FINISH_REASON_ERROR};
        p->sent = false; return COG_OK;
    }
    cog_provider_response_t response = {.content = storage, .content_capacity = capacity};
    cog_result_t rc = fake_receive(provider, &response);
    if (rc == COG_ERROR_OVERFLOW) {
        const char *chunk = p->script->chunks[p->chunk_index];
        *required = strlen(chunk) + 1U; return rc;
    }
    if (rc != COG_OK) { p->typed_finish = true; p->typed_outcome = rc;
        return fake_next(provider, out, storage, capacity, required); }
    *required = response.content_len + 1U;
    *out = (cog_provider_delta_t){.kind = COG_PROVIDER_DELTA_TEXT,
        .sequence = ++p->typed_sequence, .data = storage, .data_len = response.content_len};
    p->typed_finish = response.is_complete; return COG_OK;
}
static cog_result_t fake_error(const cog_provider_t *provider, cog_provider_error_t *out)
{
    const fake_provider_t *p = (const fake_provider_t *)provider;
    if (p == NULL || out == NULL) return COG_ERROR_NULL_PTR;
    if (p->last_error.code == COG_OK) return COG_ERROR_NOT_FOUND;
    *out = p->last_error; return COG_OK;
}

static const cog_provider_vtable_t fake_vtable = {
    fake_connect,
    fake_disconnect,
    fake_send,
    fake_receive,
    fake_poll,
    fake_cancel,
    fake_next,
    fake_error,
};

static cog_result_t fake_connect(const cog_endpoint_t *endpoint,
                                 cog_provider_t **out_provider)
{
    if (endpoint == NULL || out_provider == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    fake_provider_t *p = (fake_provider_t *)calloc(1U, sizeof *p);
    if (p == NULL) {
        return COG_ERROR_NOMEM;
    }
    p->base.vtable = &fake_vtable;
    p->script = g_current_script;
    *out_provider = &p->base;
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
    if (p->script != NULL && p->script->mode == FAKE_SEND_ERROR) {
        return p->script->send_result;
    }
    return COG_OK;
}

static cog_result_t fake_receive(cog_provider_t *provider,
                                 cog_provider_response_t *response)
{
    fake_provider_t *p = (fake_provider_t *)provider;
    if (p == NULL || response == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!p->sent) {
        return COG_ERROR_INVALID_STATE;
    }
    const fake_script_t *s = p->script;
    if (s == NULL) {
        return COG_ERROR_INVALID_STATE;
    }

    switch (s->mode) {
    case FAKE_SEND_ERROR:
        return COG_ERROR_INVALID_STATE; /* no request in flight */
    case FAKE_RECV_ERROR:
        p->last_error.code = s->recv_result;
        p->last_error.http_status = s->http_status;
        p->last_error.message = s->error_message;
        p->last_error.retryable = s->retryable;
        p->last_error.retry_after_ms = s->retry_after_ms;
        return s->recv_result;
    case FAKE_CANCEL:
        return COG_ERROR_ABORTED;
    case FAKE_SINGLE:
    case FAKE_STREAM:
    default:
        break;
    }

    const char *chunk = NULL;
    if (p->chunk_index < s->chunk_count) {
        chunk = s->chunks[p->chunk_index];
    }
    if (chunk == NULL) {
        if (response->content_capacity > 0U) {
            response->content[0] = '\0';
        }
        response->content_len = 0U;
        response->finish_reason = COG_FINISH_REASON_STOP;
        response->is_complete = true;
        return COG_OK;
    }
    size_t len = strlen(chunk);
    if (len >= response->content_capacity) {
        return COG_ERROR_OVERFLOW;
    }
    (void)memcpy(response->content, chunk, len);
    response->content[len] = '\0';
    response->content_len = len;
    p->chunk_index++;
    if (s->mode == FAKE_SINGLE || p->chunk_index >= s->chunk_count) {
        response->finish_reason = COG_FINISH_REASON_STOP;
        response->is_complete = true;
    } else {
        response->finish_reason = COG_FINISH_REASON_NONE;
        response->is_complete = false;
    }
    return COG_OK;
}

/* ------------------------------------------------------------------ */
/* Static endpoint / model tables                                      */
/* ------------------------------------------------------------------ */

static const cog_model_t models_a[] = {
    { "gpt-4o", "GPT-4o", 128000U,
      COG_MODEL_CAPABILITY_STREAMING | COG_MODEL_CAPABILITY_TOOLS,
      2.5f, 10.0f },
    { "gpt-4o-mini", "GPT-4o mini", 128000U,
      COG_MODEL_CAPABILITY_STREAMING, 0.15f, 0.6f },
};
static const cog_model_table_t model_table_a = { models_a, 2U };

static const cog_model_t models_b[] = {
    { "claude-3-5-sonnet", "Claude 3.5 Sonnet", 200000U,
      COG_MODEL_CAPABILITY_STREAMING | COG_MODEL_CAPABILITY_TOOLS |
          COG_MODEL_CAPABILITY_VISION,
      3.0f, 15.0f },
};
static const cog_model_table_t model_table_b = { models_b, 1U };

static const cog_endpoint_t endpoints[] = {
    { "openai", "https://api.openai.com/v1", "2024-01-01", NULL,
      &fake_vtable, &model_table_a, NULL, 0U },
    { "anthropic", "https://api.anthropic.com/v1", NULL, NULL,
      &fake_vtable, &model_table_b, NULL, 0U },
};
static const cog_endpoint_table_t endpoint_table = { endpoints, 2U };

/* ------------------------------------------------------------------ */
/* Request / response validation tests                                 */
/* ------------------------------------------------------------------ */

static void test_request_validate(void)
{
    CHECK(cog_provider_request_validate(NULL) == COG_ERROR_NULL_PTR);

    cog_provider_request_t req;
    (void)memset(&req, 0, sizeof req);

    /* model_id NULL -> INVALID_ARG. */
    CHECK(cog_provider_request_validate(&req) == COG_ERROR_INVALID_ARG);

    req.model_id = "gpt-4o";
    CHECK(cog_provider_request_validate(&req) == COG_OK);

    /* messages NULL with non-zero count -> INVALID_ARG. */
    req.message_count = 1U;
    CHECK(cog_provider_request_validate(&req) == COG_ERROR_INVALID_ARG);

    /* messages NULL with zero count -> OK. */
    req.message_count = 0U;
    CHECK(cog_provider_request_validate(&req) == COG_OK);

    /* tools_json NULL with non-zero len -> INVALID_ARG. */
    req.tools_json_len = 4U;
    CHECK(cog_provider_request_validate(&req) == COG_ERROR_INVALID_ARG);
    req.tools_json_len = 0U;
    CHECK(cog_provider_request_validate(&req) == COG_OK);

    /* A fully populated request validates. */
    cog_message_t msgs[2];
    (void)memset(msgs, 0, sizeof msgs);
    msgs[0].role = COG_MESSAGE_ROLE_SYSTEM;
    msgs[0].content = "be helpful";
    msgs[0].content_len = 10U;
    msgs[1].role = COG_MESSAGE_ROLE_USER;
    msgs[1].content = "hi";
    msgs[1].content_len = 2U;
    req.messages = msgs;
    req.message_count = 2U;
    req.tools_json = "[]";
    req.tools_json_len = 2U;
    CHECK(cog_provider_request_validate(&req) == COG_OK);
}

static void test_response_validate(void)
{
    CHECK(cog_provider_response_validate(NULL) == COG_ERROR_NULL_PTR);

    cog_provider_response_t resp;
    (void)memset(&resp, 0, sizeof resp);
    CHECK(cog_provider_response_validate(&resp) == COG_OK);

    /* content NULL with non-zero capacity -> INVALID_ARG. */
    resp.content_capacity = 16U;
    CHECK(cog_provider_response_validate(&resp) == COG_ERROR_INVALID_ARG);

    resp.content = (char *)"buf";
    CHECK(cog_provider_response_validate(&resp) == COG_OK);
}

/* ------------------------------------------------------------------ */
/* Endpoint / model registry tests                                     */
/* ------------------------------------------------------------------ */

static void test_endpoint_lookup(void)
{
    const cog_endpoint_t *ep = NULL;

    CHECK(cog_endpoint_table_lookup(NULL, "openai", &ep) ==
          COG_ERROR_NULL_PTR);
    CHECK(cog_endpoint_table_lookup(&endpoint_table, NULL, &ep) ==
          COG_ERROR_NULL_PTR);

    CHECK(cog_endpoint_table_lookup(&endpoint_table, "openai", &ep) ==
          COG_OK);
    CHECK(ep == &endpoints[0]);

    CHECK(cog_endpoint_table_lookup(&endpoint_table, "anthropic", &ep) ==
          COG_OK);
    CHECK(ep == &endpoints[1]);

    /* Presence-only lookup (out_endpoint NULL). */
    CHECK(cog_endpoint_table_lookup(&endpoint_table, "openai", NULL) ==
          COG_OK);

    /* Not found. */
    ep = NULL;
    CHECK(cog_endpoint_table_lookup(&endpoint_table, "nope", &ep) ==
          COG_ERROR_NOT_FOUND);
    CHECK(ep == NULL);

    /* Empty table. */
    cog_endpoint_table_t empty = { NULL, 0U };
    CHECK(cog_endpoint_table_lookup(&empty, "openai", &ep) ==
          COG_ERROR_NOT_FOUND);

    /* Malformed table: non-zero count with NULL backing array. */
    cog_endpoint_table_t bad = { NULL, 1U };
    CHECK(cog_endpoint_table_lookup(&bad, "openai", &ep) ==
          COG_ERROR_INVALID_ARG);
}

static void test_model_lookup(void)
{
    const cog_model_t *m = NULL;

    CHECK(cog_endpoint_model_lookup(NULL, "gpt-4o", &m) ==
          COG_ERROR_NULL_PTR);
    CHECK(cog_endpoint_model_lookup(&endpoints[0], NULL, &m) ==
          COG_ERROR_NULL_PTR);

    /* endpoint->models NULL -> NULL_PTR. */
    cog_endpoint_t no_models = endpoints[0];
    no_models.models = NULL;
    CHECK(cog_endpoint_model_lookup(&no_models, "gpt-4o", &m) ==
          COG_ERROR_NULL_PTR);

    CHECK(cog_endpoint_model_lookup(&endpoints[0], "gpt-4o", &m) == COG_OK);
    CHECK(m == &models_a[0]);

    CHECK(cog_endpoint_model_lookup(&endpoints[0], "gpt-4o-mini", &m) ==
          COG_OK);
    CHECK(m == &models_a[1]);

    /* Presence-only lookup. */
    CHECK(cog_endpoint_model_lookup(&endpoints[0], "gpt-4o", NULL) ==
          COG_OK);

    /* Not found. */
    m = NULL;
    CHECK(cog_endpoint_model_lookup(&endpoints[0], "nope", &m) ==
          COG_ERROR_NOT_FOUND);
    CHECK(m == NULL);

    /* Malformed model table. */
    cog_endpoint_t bad = endpoints[0];
    cog_model_table_t bad_table = { NULL, 1U };
    bad.models = &bad_table;
    CHECK(cog_endpoint_model_lookup(&bad, "gpt-4o", &m) ==
          COG_ERROR_INVALID_ARG);
}

static void test_capability(void)
{
    CHECK(!cog_model_has_capability(NULL, COG_MODEL_CAPABILITY_STREAMING));
    CHECK(cog_model_has_capability(&models_a[0],
                                   COG_MODEL_CAPABILITY_STREAMING));
    CHECK(cog_model_has_capability(&models_a[0],
                                   COG_MODEL_CAPABILITY_TOOLS));
    CHECK(!cog_model_has_capability(&models_a[0],
                                    COG_MODEL_CAPABILITY_VISION));
    CHECK(cog_model_has_capability(&models_b[0],
                                   COG_MODEL_CAPABILITY_VISION));
}

static void test_select_by_model(void)
{
    const cog_endpoint_t *ep = NULL;
    const cog_model_t *m = NULL;

    CHECK(cog_endpoint_table_select_by_model(NULL, "gpt-4o", &ep, &m) ==
          COG_ERROR_NULL_PTR);
    CHECK(cog_endpoint_table_select_by_model(&endpoint_table, NULL, &ep,
                                             &m) == COG_ERROR_NULL_PTR);

    /* Model in the first endpoint. */
    CHECK(cog_endpoint_table_select_by_model(&endpoint_table, "gpt-4o",
                                             &ep, &m) == COG_OK);
    CHECK(ep == &endpoints[0]);
    CHECK(m == &models_a[0]);

    /* Model in the second endpoint. */
    CHECK(cog_endpoint_table_select_by_model(&endpoint_table,
                                             "claude-3-5-sonnet", &ep,
                                             &m) == COG_OK);
    CHECK(ep == &endpoints[1]);
    CHECK(m == &models_b[0]);

    /* Endpoint-only and model-only selection. */
    CHECK(cog_endpoint_table_select_by_model(&endpoint_table, "gpt-4o",
                                             &ep, NULL) == COG_OK);
    CHECK(ep == &endpoints[0]);
    CHECK(cog_endpoint_table_select_by_model(&endpoint_table, "gpt-4o",
                                             NULL, &m) == COG_OK);
    CHECK(m == &models_a[0]);

    /* Not found. */
    ep = NULL;
    m = NULL;
    CHECK(cog_endpoint_table_select_by_model(&endpoint_table, "nope", &ep,
                                             &m) == COG_ERROR_NOT_FOUND);
    CHECK(ep == NULL);
    CHECK(m == NULL);
}

/* ------------------------------------------------------------------ */
/* Vtable dispatch tests                                               */
/* ------------------------------------------------------------------ */

static void test_connect_dispatch(void)
{
    cog_provider_t *prov = NULL;

    CHECK(cog_provider_connect(NULL, &prov) == COG_ERROR_NULL_PTR);
    CHECK(cog_provider_connect(&endpoints[0], NULL) == COG_ERROR_NULL_PTR);

    /* endpoint->vtable NULL -> NULL_PTR. */
    cog_endpoint_t no_vtable = endpoints[0];
    no_vtable.vtable = NULL;
    CHECK(cog_provider_connect(&no_vtable, &prov) == COG_ERROR_INVALID_ARG);

    /* Successful connect through the vtable. */
    g_current_script = NULL;
    CHECK(cog_provider_connect(&endpoints[0], &prov) == COG_OK);
    CHECK(prov != NULL);

    /* Disconnect NULL is a no-op returning COG_OK. */
    CHECK(cog_provider_disconnect(NULL) == COG_OK);

    CHECK(cog_provider_disconnect(prov) == COG_OK);
}

static void test_send_receive_dispatch(void)
{
    static const fake_script_t script = {
        .mode = FAKE_SINGLE,
        .chunks = { "hello" },
        .chunk_count = 1,
    };
    g_current_script = &script;

    cog_provider_t *prov = NULL;
    CHECK(cog_provider_connect(&endpoints[0], &prov) == COG_OK);
    CHECK(prov != NULL);

    cog_provider_request_t req;
    (void)memset(&req, 0, sizeof req);
    req.model_id = "gpt-4o";

    CHECK(cog_provider_send(NULL, &req) == COG_ERROR_NULL_PTR);
    CHECK(cog_provider_send(prov, NULL) == COG_ERROR_NULL_PTR);

    char buf[64];
    cog_provider_response_t resp;
    (void)memset(&resp, 0, sizeof resp);
    resp.content = buf;
    resp.content_capacity = sizeof buf;

    CHECK(cog_provider_receive(NULL, &resp) == COG_ERROR_NULL_PTR);
    CHECK(cog_provider_receive(prov, NULL) == COG_ERROR_NULL_PTR);

    /* Receive before send -> INVALID_STATE. */
    CHECK(cog_provider_receive(prov, &resp) == COG_ERROR_INVALID_STATE);

    CHECK(cog_provider_send(prov, &req) == COG_OK);
    CHECK(cog_provider_receive(prov, &resp) == COG_OK);
    CHECK(resp.is_complete);
    CHECK(resp.content_len == 5U);
    CHECK(memcmp(resp.content, "hello", 5U) == 0);

    CHECK(cog_provider_disconnect(prov) == COG_OK);
}

static void test_message_history(void)
{
    static const fake_script_t script = {
        .mode = FAKE_SINGLE,
        .chunks = { "ok" },
        .chunk_count = 1,
    };
    g_current_script = &script;

    cog_provider_t *prov = NULL;
    CHECK(cog_provider_connect(&endpoints[0], &prov) == COG_OK);

    cog_message_t msgs[3] = {0};
    (void)memset(msgs, 0, sizeof msgs);
    msgs[0].role = COG_MESSAGE_ROLE_SYSTEM;
    msgs[0].content = "sys";
    msgs[0].content_len = 3U;
    msgs[1].role = COG_MESSAGE_ROLE_USER;
    msgs[1].content = "user msg";
    msgs[1].content_len = 8U;
    msgs[2].role = COG_MESSAGE_ROLE_TOOL;
    msgs[2].content = "tool result";
    msgs[2].content_len = 11U;
    msgs[2].name = "calculator";
    msgs[2].tool_call_id = "call_calculator";

    cog_provider_request_t req;
    (void)memset(&req, 0, sizeof req);
    req.model_id = "gpt-4o";
    req.messages = msgs;
    req.message_count = 3U;
    req.stream = true;
    req.temperature = 0.7f;
    req.max_tokens = 256U;

    CHECK(cog_provider_send(prov, &req) == COG_OK);

    /* The fake provider must have seen the exact request pointer and
     * therefore the full message history. */
    fake_provider_t *fp = (fake_provider_t *)prov;
    CHECK(fp->last_request == &req);
    CHECK(fp->last_request->message_count == 3U);
    CHECK(fp->last_request->messages == msgs);
    CHECK(fp->last_request->messages[0].role == COG_MESSAGE_ROLE_SYSTEM);
    CHECK(fp->last_request->messages[2].role == COG_MESSAGE_ROLE_TOOL);
    CHECK(strcmp(fp->last_request->messages[2].name, "calculator") == 0);
    CHECK(fp->last_request->stream);
    CHECK(fp->last_request->max_tokens == 256U);

    CHECK(cog_provider_disconnect(prov) == COG_OK);
}

static void test_streaming(void)
{
    static const fake_script_t script = {
        .mode = FAKE_STREAM,
        .chunks = { "Hello", ", ", "world", "!" },
        .chunk_count = 4,
    };
    g_current_script = &script;

    cog_provider_t *prov = NULL;
    CHECK(cog_provider_connect(&endpoints[0], &prov) == COG_OK);

    cog_provider_request_t req;
    (void)memset(&req, 0, sizeof req);
    req.model_id = "gpt-4o";
    req.stream = true;
    CHECK(cog_provider_send(prov, &req) == COG_OK);

    char buf[64];
    char collected[64];
    size_t collected_len = 0U;
    int chunks = 0;
    bool complete = false;
    int guard = 0;
    while (!complete && guard < 32) {
        guard++;
        cog_provider_response_t resp;
        (void)memset(&resp, 0, sizeof resp);
        resp.content = buf;
        resp.content_capacity = sizeof buf;
        cog_result_t r = cog_provider_receive(prov, &resp);
        CHECK(r == COG_OK);
        if (r != COG_OK) {
            break;
        }
        if (resp.content_len > 0U) {
            if (collected_len + resp.content_len < sizeof collected) {
                (void)memcpy(collected + collected_len, resp.content,
                             resp.content_len);
                collected_len += resp.content_len;
            }
            chunks++;
        }
        complete = resp.is_complete;
    }
    CHECK(complete);
    CHECK(chunks == 4);
    CHECK(collected_len == strlen("Hello, world!"));
    CHECK(memcmp(collected, "Hello, world!", collected_len) == 0);

    CHECK(cog_provider_disconnect(prov) == COG_OK);
}

static void test_streaming_overflow(void)
{
    static const fake_script_t script = {
        .mode = FAKE_STREAM,
        .chunks = { "a very long chunk that will not fit" },
        .chunk_count = 1,
    };
    g_current_script = &script;

    cog_provider_t *prov = NULL;
    CHECK(cog_provider_connect(&endpoints[0], &prov) == COG_OK);

    cog_provider_request_t req;
    (void)memset(&req, 0, sizeof req);
    req.model_id = "gpt-4o";
    CHECK(cog_provider_send(prov, &req) == COG_OK);

    char buf[4];
    cog_provider_response_t resp;
    (void)memset(&resp, 0, sizeof resp);
    resp.content = buf;
    resp.content_capacity = sizeof buf;
    CHECK(cog_provider_receive(prov, &resp) == COG_ERROR_OVERFLOW);

    CHECK(cog_provider_disconnect(prov) == COG_OK);
}

static void test_send_error(void)
{
    static const fake_script_t script = {
        .mode = FAKE_SEND_ERROR,
        .send_result = COG_ERROR_UNAVAILABLE,
    };
    g_current_script = &script;

    cog_provider_t *prov = NULL;
    CHECK(cog_provider_connect(&endpoints[0], &prov) == COG_OK);

    cog_provider_request_t req;
    (void)memset(&req, 0, sizeof req);
    req.model_id = "gpt-4o";
    CHECK(cog_provider_send(prov, &req) == COG_ERROR_UNAVAILABLE);

    CHECK(cog_provider_disconnect(prov) == COG_OK);
}

static void test_receive_error_retry_metadata(void)
{
    static const fake_script_t script = {
        .mode = FAKE_RECV_ERROR,
        .recv_result = COG_ERROR_UNAVAILABLE,
        .http_status = 429,
        .retryable = true,
        .retry_after_ms = 3000U,
        .error_message = "rate limited",
    };
    g_current_script = &script;

    cog_provider_t *prov = NULL;
    CHECK(cog_provider_connect(&endpoints[0], &prov) == COG_OK);

    cog_provider_request_t req;
    (void)memset(&req, 0, sizeof req);
    req.model_id = "gpt-4o";
    CHECK(cog_provider_send(prov, &req) == COG_OK);

    char buf[64];
    cog_provider_response_t resp;
    (void)memset(&resp, 0, sizeof resp);
    resp.content = buf;
    resp.content_capacity = sizeof buf;
    CHECK(cog_provider_receive(prov, &resp) == COG_ERROR_UNAVAILABLE);

    fake_provider_t *fp = (fake_provider_t *)prov;
    CHECK(fp->last_error.code == COG_ERROR_UNAVAILABLE);
    CHECK(fp->last_error.http_status == 429);
    CHECK(fp->last_error.retryable);
    CHECK(fp->last_error.retry_after_ms == 3000U);
    CHECK(fp->last_error.message != NULL);
    CHECK(strcmp(fp->last_error.message, "rate limited") == 0);

    CHECK(cog_provider_disconnect(prov) == COG_OK);
}

static void test_cancel(void)
{
    static const fake_script_t script = {
        .mode = FAKE_CANCEL,
    };
    g_current_script = &script;

    cog_provider_t *prov = NULL;
    CHECK(cog_provider_connect(&endpoints[0], &prov) == COG_OK);

    cog_provider_request_t req;
    (void)memset(&req, 0, sizeof req);
    req.model_id = "gpt-4o";
    req.stream = true;
    CHECK(cog_provider_send(prov, &req) == COG_OK);

    char buf[64];
    cog_provider_response_t resp;
    (void)memset(&resp, 0, sizeof resp);
    resp.content = buf;
    resp.content_capacity = sizeof buf;
    CHECK(cog_provider_receive(prov, &resp) == COG_ERROR_ABORTED);

    CHECK(cog_provider_disconnect(prov) == COG_OK);
}

/* Legacy HTTP-only provider fake retired: it used implicit cog_net_new(NULL),
 * HTTP and consume-on-poll. Actual adapter codec tests live in test_openrouter;
 * real audited HTTPS integration is separately registered by the provider. */

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    test_request_validate();
    test_response_validate();
    test_endpoint_lookup();
    test_model_lookup();
    test_capability();
    test_select_by_model();
    test_connect_dispatch();
    test_send_receive_dispatch();
    test_message_history();
    test_streaming();
    test_streaming_overflow();
    test_send_error();
    test_receive_error_retry_metadata();
    test_cancel();

    (void)printf("test_provider: %d checks, %d failures\n", g_checks,
                 g_failures);
    return g_failures == 0 ? 0 : 1;
}
