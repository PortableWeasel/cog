/**
 * @file test_provider.c
 * @brief Contract test for cog/provider.h, provider_request.h,
 *        provider_vtable.h and endpoint.h (libraries/cog-provider).
 *
 * Drives the public cog-provider API with fake static vtables and a local
 * loopback HTTP server (fork-per-connection, no external network).  Covers:
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

static uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static void sleep_ms(long ms)
{
    if (ms <= 0L) {
        return;
    }
    struct timespec ts;
    ts.tv_sec = ms / 1000L;
    ts.tv_nsec = (ms % 1000L) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

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

static const cog_provider_vtable_t fake_vtable = {
    fake_connect,
    fake_disconnect,
    fake_send,
    fake_receive,
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
/* Net-backed fake provider (real transport through cog-net)           */
/* ------------------------------------------------------------------ */

typedef struct net_provider {
    struct cog_provider base;
    cog_net_ctx_t *net;
    char base_url[256];
    cog_request_id_t req_id;
    bool sent;
    bool headers_seen;
    bool complete;
    int http_status;
    char headers[512];
    char retry_after[32];
} net_provider_t;

static cog_result_t net_connect(const cog_endpoint_t *endpoint,
                                cog_provider_t **out_provider);
static cog_result_t net_disconnect(cog_provider_t *provider);
static cog_result_t net_send(cog_provider_t *provider,
                             const cog_provider_request_t *request);
static cog_result_t net_receive(cog_provider_t *provider,
                                cog_provider_response_t *response);

static const cog_provider_vtable_t net_vtable = {
    net_connect,
    net_disconnect,
    net_send,
    net_receive,
};

static cog_result_t net_connect(const cog_endpoint_t *endpoint,
                                cog_provider_t **out_provider)
{
    if (endpoint == NULL || out_provider == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    net_provider_t *p = (net_provider_t *)calloc(1U, sizeof *p);
    if (p == NULL) {
        return COG_ERROR_NOMEM;
    }
    p->base.vtable = &net_vtable;
    if (endpoint->base_url != NULL) {
        int n = snprintf(p->base_url, sizeof p->base_url, "%s",
                         endpoint->base_url);
        if (n < 0 || (size_t)n >= sizeof p->base_url) {
            free(p);
            return COG_ERROR_INVALID_ARG;
        }
    }
    cog_result_t r = cog_net_new(&p->net, NULL);
    if (r != COG_OK) {
        free(p);
        return r;
    }
    *out_provider = &p->base;
    return COG_OK;
}

static cog_result_t net_disconnect(cog_provider_t *provider)
{
    net_provider_t *p = (net_provider_t *)provider;
    if (p == NULL) {
        return COG_OK;
    }
    cog_net_free(p->net);
    free(p);
    return COG_OK;
}

static cog_result_t net_send(cog_provider_t *provider,
                             const cog_provider_request_t *request)
{
    net_provider_t *p = (net_provider_t *)provider;
    if (p == NULL || request == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (p->sent) {
        return COG_ERROR_INVALID_STATE;
    }
    /* The model_id selects the server path (e.g. "/stream"). */
    const char *path = request->model_id != NULL ? request->model_id : "/";
    char url[512];
    int n = snprintf(url, sizeof url, "%s%s", p->base_url, path);
    if (n < 0 || (size_t)n >= sizeof url) {
        return COG_ERROR_INVALID_ARG;
    }
    static const char body[] = "{\"model\":\"test\"}";
    cog_result_t r = cog_net_request(p->net, "POST", url, NULL, body,
                                     sizeof body - 1U, &p->req_id);
    if (r != COG_OK) {
        return r;
    }
    p->sent = true;
    return COG_OK;
}

static cog_result_t net_receive(cog_provider_t *provider,
                                cog_provider_response_t *response)
{
    net_provider_t *p = (net_provider_t *)provider;
    if (p == NULL || response == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (!p->sent) {
        return COG_ERROR_INVALID_STATE;
    }
    if (p->complete) {
        return COG_ERROR_INVALID_STATE;
    }

    cog_net_response_t nr;
    cog_result_t r = cog_net_poll(p->net, &nr);
    if (r == COG_ERROR_AGAIN) {
        return COG_ERROR_AGAIN;
    }
    if (r != COG_OK) {
        return r;
    }
    if (nr.headers != NULL && !p->headers_seen) {
        p->headers_seen = true;
        p->http_status = nr.http_status;
        int hn = snprintf(p->headers, sizeof p->headers, "%s", nr.headers);
        if (hn < 0 || (size_t)hn >= sizeof p->headers) {
            p->headers[sizeof p->headers - 1U] = '\0';
        }
        const char *ra = strstr(p->headers, "Retry-After:");
        if (ra != NULL) {
            const char *v = ra + strlen("Retry-After:");
            while (*v == ' ' || *v == '\t') {
                v++;
            }
            size_t i = 0U;
            while (v[i] >= '0' && v[i] <= '9' &&
                   i + 1U < sizeof p->retry_after) {
                p->retry_after[i] = v[i];
                i++;
            }
            p->retry_after[i] = '\0';
        }
    }
    if (nr.chunk != NULL && nr.chunk_len > 0U) {
        if (nr.chunk_len >= response->content_capacity) {
            return COG_ERROR_OVERFLOW;
        }
        (void)memcpy(response->content, nr.chunk, nr.chunk_len);
        response->content[nr.chunk_len] = '\0';
        response->content_len = nr.chunk_len;
    } else {
        response->content_len = 0U;
        if (response->content_capacity > 0U) {
            response->content[0] = '\0';
        }
    }
    response->is_complete = nr.is_complete;
    if (nr.is_complete) {
        response->finish_reason = COG_FINISH_REASON_STOP;
        p->complete = true;
    } else {
        response->finish_reason = COG_FINISH_REASON_NONE;
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
      &fake_vtable, &model_table_a },
    { "anthropic", "https://api.anthropic.com/v1", NULL, NULL,
      &fake_vtable, &model_table_b },
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
    CHECK(cog_provider_connect(&no_vtable, &prov) == COG_ERROR_NULL_PTR);

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

    cog_message_t msgs[3];
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

/* ------------------------------------------------------------------ */
/* Loopback HTTP test server                                           */
/* ------------------------------------------------------------------ */

#define REQUEST_CAP 4096U
#define PATH_CAP 512U
#define LISTEN_BACKLOG 16

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t off = 0U;
    while (off < len) {
        ssize_t w = send(fd, p + off, len - off, 0);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static int send_simple(int fd, int status, const char *reason)
{
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, reason);
    if (n < 0 || (size_t)n >= sizeof head) {
        return -1;
    }
    return write_all(fd, head, (size_t)n);
}

static int send_stream_head(int fd)
{
    static const char head[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n";
    return write_all(fd, head, sizeof head - 1U);
}

static int write_chunk(int fd, const char *payload)
{
    size_t plen = strlen(payload);
    char hdr[32];
    int n = snprintf(hdr, sizeof hdr, "%zx\r\n", plen);
    if (n < 0 || (size_t)n >= sizeof hdr) {
        return -1;
    }
    if (write_all(fd, hdr, (size_t)n) != 0) {
        return -1;
    }
    if (write_all(fd, payload, plen) != 0) {
        return -1;
    }
    return write_all(fd, "\r\n", 2U);
}

static int write_last_chunk(int fd)
{
    return write_all(fd, "0\r\n\r\n", 5U);
}

static int handle_stream(int fd)
{
    static const char *parts[] = { "Hello", ", ", "world", "!" };
    size_t i;
    for (i = 0U; i < sizeof parts / sizeof parts[0]; i++) {
        if (write_chunk(fd, parts[i]) != 0) {
            return -1;
        }
        sleep_ms(15L);
    }
    return write_last_chunk(fd);
}

static int handle_rate(int fd)
{
    static const char head[] =
        "HTTP/1.1 429 Too Many Requests\r\n"
        "Retry-After: 3\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";
    return write_all(fd, head, sizeof head - 1U);
}

static int handle_malformed(int fd)
{
    static const char garbage[] = "GARBAGE NOT HTTP\r\n\r\n";
    return write_all(fd, garbage, sizeof garbage - 1U);
}

static int read_request(int fd, uint8_t *buf, size_t cap, size_t *len_out)
{
    size_t got = 0U;
    while (got < cap) {
        ssize_t n = recv(fd, buf + got, cap - got, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        got += (size_t)n;
        size_t i;
        for (i = 0U; i + 4U <= got; i++) {
            if (memcmp(buf + i, "\r\n\r\n", 4U) == 0) {
                *len_out = got;
                return 0;
            }
        }
    }
    return -1;
}

static int parse_target(const uint8_t *buf, size_t len, char *path,
                        size_t path_cap)
{
    if (len < 6U || (memcmp(buf, "GET ", 4U) != 0 &&
                     memcmp(buf, "POST", 4U) != 0)) {
        return -1;
    }
    size_t line_end = 0U;
    while (line_end < len && buf[line_end] != '\n') {
        line_end++;
    }
    if (line_end >= len) {
        return -1;
    }
    size_t sp1 = 0U;
    while (sp1 < line_end && buf[sp1] != ' ') {
        sp1++;
    }
    size_t sp2 = sp1 + 1U;
    while (sp2 < line_end && buf[sp2] != ' ') {
        sp2++;
    }
    if (sp1 >= line_end || sp2 >= line_end || sp2 <= sp1 + 1U) {
        return -1;
    }
    size_t plen = sp2 - (sp1 + 1U);
    if (plen == 0U || plen >= path_cap) {
        return -1;
    }
    (void)memcpy(path, buf + sp1 + 1U, plen);
    path[plen] = '\0';
    return 0;
}

static void handle_connection(int fd)
{
    uint8_t req_buf[REQUEST_CAP];
    size_t req_len = 0U;
    if (read_request(fd, req_buf, sizeof req_buf, &req_len) != 0) {
        (void)close(fd);
        return;
    }
    char target[PATH_CAP];
    if (parse_target(req_buf, req_len, target, sizeof target) != 0) {
        (void)send_simple(fd, 400, "Bad Request");
        (void)close(fd);
        return;
    }
    char *query = strchr(target, '?');
    if (query != NULL) {
        *query = '\0';
    }

    int rc;
    if (strcmp(target, "/stream") == 0) {
        (void)send_stream_head(fd);
        rc = handle_stream(fd);
    } else if (strcmp(target, "/rate") == 0) {
        rc = handle_rate(fd);
    } else if (strcmp(target, "/malformed") == 0) {
        rc = handle_malformed(fd);
    } else {
        rc = send_simple(fd, 404, "Not Found");
    }
    (void)rc;
    (void)close(fd);
}

static int bind_listener(uint16_t *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one,
                     (socklen_t)sizeof one);
    struct sockaddr_in addr;
    (void)memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0U;
    if (bind(fd, (struct sockaddr *)&addr, (socklen_t)sizeof addr) != 0) {
        (void)close(fd);
        return -1;
    }
    socklen_t alen = (socklen_t)sizeof addr;
    if (getsockname(fd, (struct sockaddr *)&addr, &alen) != 0) {
        (void)close(fd);
        return -1;
    }
    *port = ntohs(addr.sin_port);
    if (listen(fd, LISTEN_BACKLOG) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

static void server_loop(int listen_fd)
{
    (void)signal(SIGPIPE, SIG_IGN);
    (void)signal(SIGCHLD, SIG_IGN);
    for (;;) {
        int c = accept(listen_fd, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        pid_t pid = fork();
        if (pid < 0) {
            (void)close(c);
            continue;
        }
        if (pid == 0) {
            (void)close(listen_fd);
            handle_connection(c);
            (void)close(c);
            _exit(0);
        }
        (void)close(c);
    }
}

static int spawn_server(uint16_t *port, pid_t *pid)
{
    int fd = bind_listener(port);
    if (fd < 0) {
        return -1;
    }
    pid_t p = fork();
    if (p < 0) {
        (void)close(fd);
        return -1;
    }
    if (p == 0) {
        server_loop(fd);
        _exit(0);
    }
    (void)close(fd);
    *pid = p;
    return 0;
}

static void stop_server(pid_t pid)
{
    if (pid > 0) {
        (void)kill(pid, SIGTERM);
        (void)waitpid(pid, NULL, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Net transport tests (real cog-net path)                             */
/* ------------------------------------------------------------------ */

typedef struct collect {
    char data[4096];
    size_t len;
    int status;
    bool got_headers;
    char headers[1024];
    int chunks;
} collect_t;

static void collect_init(collect_t *c)
{
    (void)memset(c, 0, sizeof *c);
}

/* The endpoint's base_url is copied by net_connect, but the string must
 * remain valid until connect runs, so it lives in a static buffer. */
static char g_net_base_url[64];

static cog_endpoint_t make_net_endpoint(uint16_t port)
{
    cog_endpoint_t ep;
    (void)memset(&ep, 0, sizeof ep);
    ep.name = "loopback";
    ep.vtable = &net_vtable;
    ep.models = &model_table_a;
    int n = snprintf(g_net_base_url, sizeof g_net_base_url,
                     "http://127.0.0.1:%u", (unsigned)port);
    if (n < 0 || (size_t)n >= sizeof g_net_base_url) {
        g_net_base_url[0] = '\0';
    }
    ep.base_url = g_net_base_url;
    return ep;
}

static cog_result_t provider_drain(cog_provider_t *prov, collect_t *c,
                                   uint64_t timeout_ms)
{
    uint64_t start = now_ms();
    char buf[1024];
    while (now_ms() - start < timeout_ms) {
        cog_provider_response_t resp;
        (void)memset(&resp, 0, sizeof resp);
        resp.content = buf;
        resp.content_capacity = sizeof buf;
        cog_result_t r = cog_provider_receive(prov, &resp);
        if (r == COG_OK) {
            if (resp.content_len > 0U) {
                if (c->len + resp.content_len < sizeof c->data) {
                    (void)memcpy(c->data + c->len, resp.content,
                                 resp.content_len);
                    c->len += resp.content_len;
                }
                c->chunks++;
            }
            if (resp.is_complete) {
                return COG_OK;
            }
        } else if (r != COG_ERROR_AGAIN) {
            return r;
        }
        sleep_ms(1);
    }
    return COG_ERROR_TIMEOUT;
}

static void test_net_streaming(uint16_t port)
{
    cog_endpoint_t ep = make_net_endpoint(port);
    cog_provider_t *prov = NULL;
    CHECK(cog_provider_connect(&ep, &prov) == COG_OK);
    CHECK(prov != NULL);

    cog_provider_request_t req;
    (void)memset(&req, 0, sizeof req);
    req.model_id = "/stream";
    req.stream = true;
    CHECK(cog_provider_send(prov, &req) == COG_OK);

    collect_t c;
    collect_init(&c);
    cog_result_t r = provider_drain(prov, &c, 5000U);
    CHECK(r == COG_OK);
    CHECK(c.len == strlen("Hello, world!"));
    CHECK(memcmp(c.data, "Hello, world!", c.len) == 0);
    CHECK(c.chunks >= 4);

    net_provider_t *np = (net_provider_t *)prov;
    CHECK(np->http_status == 200);

    CHECK(cog_provider_disconnect(prov) == COG_OK);
}

static void test_net_rate_limit(uint16_t port)
{
    cog_endpoint_t ep = make_net_endpoint(port);
    cog_provider_t *prov = NULL;
    CHECK(cog_provider_connect(&ep, &prov) == COG_OK);

    cog_provider_request_t req;
    (void)memset(&req, 0, sizeof req);
    req.model_id = "/rate";
    CHECK(cog_provider_send(prov, &req) == COG_OK);

    collect_t c;
    collect_init(&c);
    cog_result_t r = provider_drain(prov, &c, 5000U);
    CHECK(r == COG_OK);

    net_provider_t *np = (net_provider_t *)prov;
    CHECK(np->http_status == 429);
    CHECK(strcmp(np->retry_after, "3") == 0);

    CHECK(cog_provider_disconnect(prov) == COG_OK);
}

static void test_net_malformed(uint16_t port)
{
    cog_endpoint_t ep = make_net_endpoint(port);
    cog_provider_t *prov = NULL;
    CHECK(cog_provider_connect(&ep, &prov) == COG_OK);

    cog_provider_request_t req;
    (void)memset(&req, 0, sizeof req);
    req.model_id = "/malformed";
    CHECK(cog_provider_send(prov, &req) == COG_OK);

    collect_t c;
    collect_init(&c);
    cog_result_t r = provider_drain(prov, &c, 3000U);
    CHECK(r == COG_ERROR_IO);

    CHECK(cog_provider_disconnect(prov) == COG_OK);
}

static void test_net_overflow(uint16_t port)
{
    cog_endpoint_t ep = make_net_endpoint(port);
    cog_provider_t *prov = NULL;
    CHECK(cog_provider_connect(&ep, &prov) == COG_OK);

    cog_provider_request_t req;
    (void)memset(&req, 0, sizeof req);
    req.model_id = "/stream";
    req.stream = true;
    CHECK(cog_provider_send(prov, &req) == COG_OK);

    /* A tiny buffer cannot hold a chunk -> OVERFLOW. */
    char tiny[2];
    cog_provider_response_t resp;
    (void)memset(&resp, 0, sizeof resp);
    resp.content = tiny;
    resp.content_capacity = sizeof tiny;
    cog_result_t r = COG_ERROR_AGAIN;
    uint64_t start = now_ms();
    while (now_ms() - start < 3000U) {
        r = cog_provider_receive(prov, &resp);
        /* Skip the headers node (COG_OK, no chunk) and would-block; stop
         * on the first real error. */
        if (r != COG_ERROR_AGAIN && r != COG_OK) {
            break;
        }
        sleep_ms(1);
    }
    CHECK(r == COG_ERROR_OVERFLOW);

    CHECK(cog_provider_disconnect(prov) == COG_OK);
}

static void test_net_cancel(uint16_t port)
{
    cog_endpoint_t ep = make_net_endpoint(port);
    cog_provider_t *prov = NULL;
    CHECK(cog_provider_connect(&ep, &prov) == COG_OK);

    cog_provider_request_t req;
    (void)memset(&req, 0, sizeof req);
    req.model_id = "/stream";
    req.stream = true;
    CHECK(cog_provider_send(prov, &req) == COG_OK);

    /* Cancel the underlying transport request directly (white-box: the
     * test knows the net_provider layout). */
    net_provider_t *np = (net_provider_t *)prov;
    CHECK(cog_net_cancel(np->net, np->req_id) == COG_OK);

    char buf[64];
    cog_provider_response_t resp;
    (void)memset(&resp, 0, sizeof resp);
    resp.content = buf;
    resp.content_capacity = sizeof buf;
    cog_result_t r = COG_ERROR_AGAIN;
    uint64_t start = now_ms();
    while (now_ms() - start < 3000U) {
        r = cog_provider_receive(prov, &resp);
        if (r != COG_ERROR_AGAIN) {
            break;
        }
        sleep_ms(1);
    }
    CHECK(r == COG_ERROR_ABORTED);

    CHECK(cog_provider_disconnect(prov) == COG_OK);
}

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

    uint16_t port = 0U;
    pid_t server = 0;
    if (spawn_server(&port, &server) != 0) {
        (void)fprintf(stderr, "failed to spawn test server\n");
        return 1;
    }
    test_net_streaming(port);
    test_net_rate_limit(port);
    test_net_malformed(port);
    test_net_overflow(port);
    test_net_cancel(port);
    stop_server(server);

    (void)printf("test_provider: %d checks, %d failures\n", g_checks,
                 g_failures);
    return g_failures == 0 ? 0 : 1;
}
