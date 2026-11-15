/**
 * @file test_provider_critic.c
 * @brief Critic adversarial contract test for cog-provider (ticket c42b7ca).
 *
 * Complements tests/unit/test_provider.c by pinning gaps the Writer's
 * suite did not cover:
 *
 *   - out-parameter preservation on failure (sentinel unchanged on
 *     NULL_PTR / INVALID_ARG / NOT_FOUND)
 *   - exact vtable call counts (connect/disconnect/send/receive each
 *     dispatched exactly once per wrapper call)
 *   - connect / disconnect error propagation through the wrappers
 *   - select_by_model with empty tables, NULL endpoint rows, and
 *     malformed model tables (skip, not crash)
 *   - endpoint lookup with a NULL-name row (skipped)
 *   - response validation boundary (content non-NULL, capacity 0)
 *   - request validation boundary (messages/tools NULL with zero counts)
 *
 * Ticket: c42b7ca (Stage 12.4a — Review provider implementation)
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/provider.h"

#include "provider_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
/* Counting / error-injecting fake vtable                              */
/* ------------------------------------------------------------------ */

typedef struct counting_provider {
    struct cog_provider base;
    int *connect_calls;
    int *disconnect_calls;
    int *send_calls;
    int *receive_calls;
    cog_result_t connect_result;
    cog_result_t disconnect_result;
    cog_result_t send_result;
    cog_result_t receive_result;
} counting_provider_t;

static cog_result_t counting_connect(const cog_endpoint_t *endpoint,
                                     cog_provider_t **out_provider);
static cog_result_t counting_disconnect(cog_provider_t *provider);
static cog_result_t counting_send(cog_provider_t *provider,
                                  const cog_provider_request_t *request);
static cog_result_t counting_receive(cog_provider_t *provider,
                                     cog_provider_response_t *response);

static cog_result_t counting_poll(cog_provider_t *p,uint64_t now,uint64_t *deadline)
{ (void)now; if(p==NULL||deadline==NULL)return COG_ERROR_NULL_PTR;*deadline=UINT64_MAX;return ((counting_provider_t *)p)->receive_result; }
static cog_result_t counting_cancel(cog_provider_t *p)
{ if(p==NULL)return COG_ERROR_NULL_PTR;return ((counting_provider_t *)p)->receive_result; }
static cog_result_t counting_next(cog_provider_t *p,cog_provider_delta_t *d,void *s,size_t n,size_t *required)
{ (void)s;(void)n;if(p==NULL||d==NULL||required==NULL)return COG_ERROR_NULL_PTR;*required=0U;return COG_ERROR_AGAIN; }
static cog_result_t counting_error(const cog_provider_t *p,cog_provider_error_t *e)
{ if(p==NULL||e==NULL)return COG_ERROR_NULL_PTR;return COG_ERROR_NOT_FOUND; }
static cog_result_t counting_preflight(cog_provider_t *p,
    const cog_provider_request_t *r, cog_provider_preflight_stage_t stage)
{
    (void)stage;
    if (p == NULL || r == NULL) return COG_ERROR_NULL_PTR;
    return COG_OK;
}
static const cog_provider_vtable_t counting_vtable = {
    counting_connect,
    counting_disconnect,
    counting_send,
    counting_receive,
    counting_poll, counting_cancel, counting_next, counting_error, counting_preflight,
};

/* The script for the next counting_connect.  Single-threaded test. */
static counting_provider_t *g_next = NULL;

static cog_result_t counting_connect(const cog_endpoint_t *endpoint,
                                     cog_provider_t **out_provider)
{
    if (endpoint == NULL || out_provider == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (g_next == NULL) {
        return COG_ERROR_INVALID_STATE;
    }
    counting_provider_t *p = g_next;
    g_next = NULL;
    (*p->connect_calls)++;
    if (p->connect_result != COG_OK) {
        /* Deliberately leave the backend output untouched: generic connect
         * must still publish NULL on every failure. */
        return p->connect_result;
    }
    *out_provider = &p->base;
    return COG_OK;
}

static cog_result_t counting_disconnect(cog_provider_t *provider)
{
    counting_provider_t *p = (counting_provider_t *)provider;
    if (p == NULL) {
        return COG_OK;
    }
    (*p->disconnect_calls)++;
    return p->disconnect_result;
}

static cog_result_t counting_send(cog_provider_t *provider,
                                  const cog_provider_request_t *request)
{
    counting_provider_t *p = (counting_provider_t *)provider;
    if (p == NULL || request == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    (*p->send_calls)++;
    return p->send_result;
}

static cog_result_t counting_receive(cog_provider_t *provider,
                                     cog_provider_response_t *response)
{
    counting_provider_t *p = (counting_provider_t *)provider;
    if (p == NULL || response == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    (*p->receive_calls)++;
    return p->receive_result;
}

/* ------------------------------------------------------------------ */
/* Static tables for registry tests                                    */
/* ------------------------------------------------------------------ */

static const cog_model_t critic_models[] = {
    { "m1", "Model 1", 8192U, COG_MODEL_CAPABILITY_STREAMING, 1.0f, 2.0f },
};
static const cog_model_table_t critic_model_table = { critic_models, 1U };

static const cog_endpoint_t critic_endpoints[] = {
    { "ep-a", NULL, NULL, NULL, &counting_vtable, &critic_model_table, NULL, 0U },
    { "ep-b", NULL, NULL, NULL, &counting_vtable, &critic_model_table, NULL, 0U },
};
static const cog_endpoint_table_t critic_endpoint_table = {
    critic_endpoints, 2U,
};

/* ------------------------------------------------------------------ */
/* Out-parameter preservation                                          */
/* ------------------------------------------------------------------ */

static void test_out_param_preservation(void)
{
    const cog_endpoint_t *ep = (const cog_endpoint_t *)&critic_endpoints[0];
    const cog_model_t *m = (const cog_model_t *)&critic_models[0];

    /* NOT_FOUND leaves out_* unchanged. */
    CHECK(cog_endpoint_table_lookup(&critic_endpoint_table, "nope", &ep) ==
          COG_ERROR_NOT_FOUND);
    CHECK(ep == &critic_endpoints[0]);

    CHECK(cog_endpoint_model_lookup(&critic_endpoints[0], "nope", &m) ==
          COG_ERROR_NOT_FOUND);
    CHECK(m == &critic_models[0]);

    CHECK(cog_endpoint_table_select_by_model(&critic_endpoint_table, "nope",
                                             &ep, &m) == COG_ERROR_NOT_FOUND);
    CHECK(ep == &critic_endpoints[0]);
    CHECK(m == &critic_models[0]);

    /* NULL_PTR leaves out_* unchanged. */
    CHECK(cog_endpoint_table_lookup(NULL, "ep-a", &ep) ==
          COG_ERROR_NULL_PTR);
    CHECK(ep == &critic_endpoints[0]);
    CHECK(cog_endpoint_table_lookup(&critic_endpoint_table, NULL, &ep) ==
          COG_ERROR_NULL_PTR);
    CHECK(ep == &critic_endpoints[0]);

    CHECK(cog_endpoint_model_lookup(NULL, "m1", &m) == COG_ERROR_NULL_PTR);
    CHECK(m == &critic_models[0]);
    CHECK(cog_endpoint_model_lookup(&critic_endpoints[0], NULL, &m) ==
          COG_ERROR_NULL_PTR);
    CHECK(m == &critic_models[0]);

    CHECK(cog_endpoint_table_select_by_model(NULL, "m1", &ep, &m) ==
          COG_ERROR_NULL_PTR);
    CHECK(ep == &critic_endpoints[0]);
    CHECK(m == &critic_models[0]);
    CHECK(cog_endpoint_table_select_by_model(&critic_endpoint_table, NULL,
                                             &ep, &m) == COG_ERROR_NULL_PTR);
    CHECK(ep == &critic_endpoints[0]);
    CHECK(m == &critic_models[0]);

    /* INVALID_ARG (malformed table) leaves out_* unchanged. */
    cog_endpoint_table_t bad = { NULL, 1U };
    CHECK(cog_endpoint_table_lookup(&bad, "ep-a", &ep) ==
          COG_ERROR_INVALID_ARG);
    CHECK(ep == &critic_endpoints[0]);
    CHECK(cog_endpoint_table_select_by_model(&bad, "m1", &ep, &m) ==
          COG_ERROR_INVALID_ARG);
    CHECK(ep == &critic_endpoints[0]);
    CHECK(m == &critic_models[0]);

    cog_endpoint_t bad_models = critic_endpoints[0];
    cog_model_table_t bad_mt = { NULL, 1U };
    bad_models.models = &bad_mt;
    CHECK(cog_endpoint_model_lookup(&bad_models, "m1", &m) ==
          COG_ERROR_INVALID_ARG);
    CHECK(m == &critic_models[0]);
}

/* ------------------------------------------------------------------ */
/* Exact vtable call counts and error propagation                      */
/* ------------------------------------------------------------------ */

static void test_call_counts_and_error_propagation(void)
{
    int cc = 0, dc = 0, sc = 0, rc = 0;
    counting_provider_t p;
    (void)memset(&p, 0, sizeof p);
    p.base.vtable = &counting_vtable;
    p.connect_calls = &cc;
    p.disconnect_calls = &dc;
    p.send_calls = &sc;
    p.receive_calls = &rc;
    p.connect_result = COG_OK;
    p.disconnect_result = COG_OK;
    p.send_result = COG_OK;
    p.receive_result = COG_OK;

    /* Successful connect dispatches exactly once. */
    g_next = &p;
    cog_provider_t *prov = NULL;
    CHECK(cog_provider_connect(&critic_endpoints[0], &prov) == COG_OK);
    CHECK(cc == 1);
    CHECK(prov == &p.base);

    /* send / receive each dispatch exactly once and propagate results. */
    cog_provider_request_t req;
    (void)memset(&req, 0, sizeof req);
    req.model_id = "m1";
    char buf[8];
    cog_provider_response_t resp;
    (void)memset(&resp, 0, sizeof resp);
    resp.content = buf;
    resp.content_capacity = sizeof buf;

    CHECK(cog_provider_send(prov, &req) == COG_OK);
    CHECK(sc == 1);
    CHECK(cog_provider_receive(prov, &resp) == COG_OK);
    CHECK(rc == 1);

    /* send error propagates verbatim, dispatched once. */
    p.send_result = COG_ERROR_UNAVAILABLE;
    CHECK(cog_provider_send(prov, &req) == COG_ERROR_UNAVAILABLE);
    CHECK(sc == 2);

    /* receive error propagates verbatim, dispatched once. */
    p.receive_result = COG_ERROR_ABORTED;
    CHECK(cog_provider_receive(prov, &resp) == COG_ERROR_ABORTED);
    CHECK(rc == 2);

    /* disconnect error propagates verbatim, dispatched once. */
    p.disconnect_result = COG_ERROR_IO;
    CHECK(cog_provider_disconnect(prov) == COG_ERROR_IO);
    CHECK(dc == 1);

    /* NULL provider is a no-op: no dispatch. */
    CHECK(cog_provider_disconnect(NULL) == COG_OK);
    CHECK(dc == 1);
    CHECK(cog_provider_send(NULL, &req) == COG_ERROR_NULL_PTR);
    CHECK(sc == 2); /* unchanged */
    CHECK(cog_provider_receive(NULL, &resp) == COG_ERROR_NULL_PTR);
    CHECK(rc == 2); /* unchanged */
}

static void test_connect_failure_propagation(void)
{
    int cc = 0, dc = 0, sc = 0, rc = 0;
    counting_provider_t p;
    (void)memset(&p, 0, sizeof p);
    p.base.vtable = &counting_vtable;
    p.connect_calls = &cc;
    p.disconnect_calls = &dc;
    p.send_calls = &sc;
    p.receive_calls = &rc;
    p.connect_result = COG_ERROR_NOMEM;

    /* The wrapper forwards the provider's connect result verbatim. */
    g_next = &p;
    cog_provider_t *prov = (cog_provider_t *)&p;
    CHECK(cog_provider_connect(&critic_endpoints[0], &prov) ==
          COG_ERROR_NOMEM);
    CHECK(cc == 1);
    /* The wrapper does not itself zero *out_provider; the provider's
     * connect_fn is responsible for that per its @post.  Document the
     * actual behavior: the pointer is left as the caller set it. */
    CHECK(prov == NULL);
}

/* ------------------------------------------------------------------ */
/* select_by_model edge cases                                          */
/* ------------------------------------------------------------------ */

static void test_select_by_model_edge_cases(void)
{
    const cog_endpoint_t *ep = NULL;
    const cog_model_t *m = NULL;

    /* Empty endpoint table -> NOT_FOUND. */
    cog_endpoint_table_t empty = { NULL, 0U };
    CHECK(cog_endpoint_table_select_by_model(&empty, "m1", &ep, &m) ==
          COG_ERROR_NOT_FOUND);

    /* Malformed model table (count>0, models NULL) is skipped, not
     * dereferenced; a later good endpoint still resolves. */
    cog_model_table_t bad_mt = { NULL, 1U };
    cog_endpoint_t bad_ep = critic_endpoints[0];
    bad_ep.models = &bad_mt;
    cog_endpoint_t rows2[2];
    rows2[0] = bad_ep;
    rows2[1] = critic_endpoints[1];
    cog_endpoint_table_t with_bad_models = { rows2, 2U };
    CHECK(cog_endpoint_table_select_by_model(&with_bad_models, "m1", &ep,
                                             &m) == COG_OK);
    CHECK(ep == &rows2[1]);
    CHECK(m == &critic_models[0]);

    /* All endpoints malformed -> NOT_FOUND (no crash). */
    cog_endpoint_t rows3[1];
    rows3[0] = bad_ep;
    cog_endpoint_table_t all_bad = { rows3, 1U };
    CHECK(cog_endpoint_table_select_by_model(&all_bad, "m1", &ep, &m) ==
          COG_ERROR_NOT_FOUND);
}

/* ------------------------------------------------------------------ */
/* Endpoint lookup with NULL-name row                                  */
/* ------------------------------------------------------------------ */

static void test_null_name_row(void)
{
    cog_endpoint_t rows[2];
    rows[0] = critic_endpoints[0];
    rows[0].name = NULL; /* unnamed row must be skipped, not matched */
    rows[1] = critic_endpoints[1];
    cog_endpoint_table_t t = { rows, 2U };

    const cog_endpoint_t *ep = NULL;
    CHECK(cog_endpoint_table_lookup(&t, "ep-b", &ep) == COG_OK);
    CHECK(ep == &rows[1]);

    /* A NULL query name is rejected before any row is examined. */
    CHECK(cog_endpoint_table_lookup(&t, NULL, &ep) == COG_ERROR_NULL_PTR);
}

/* ------------------------------------------------------------------ */
/* Validation boundaries                                               */
/* ------------------------------------------------------------------ */

static void test_validation_boundaries(void)
{
    /* Response: content non-NULL with capacity 0 is valid. */
    cog_provider_response_t resp;
    (void)memset(&resp, 0, sizeof resp);
    resp.content = (char *)"x";
    resp.content_capacity = 0U;
    CHECK(cog_provider_response_validate(&resp) == COG_OK);

    /* Request: messages NULL with zero count is valid; tools NULL with
     * zero len is valid. */
    cog_provider_request_t req;
    (void)memset(&req, 0, sizeof req);
    req.model_id = "m1";
    req.messages = NULL;
    req.message_count = 0U;
    req.tools_json = NULL;
    req.tools_json_len = 0U;
    CHECK(cog_provider_request_validate(&req) == COG_OK);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void test_required_preflight(void)
{
    /* Intentionally incomplete static implementation: reject before connect. */
    static const cog_provider_vtable_t missing = {
        counting_connect, counting_disconnect, counting_send, counting_receive,
        counting_poll, counting_cancel, counting_next, counting_error, NULL};
    cog_endpoint_t endpoint = {.vtable = &missing};
    cog_provider_t *out = (cog_provider_t *)(uintptr_t)1U;
    CHECK(cog_provider_connect(&endpoint, &out) == COG_ERROR_INVALID_ARG);
    CHECK(out == NULL);
    cog_provider_t instance = {&missing};
    cog_provider_request_t request = {.model_id = "m1"};
    CHECK(cog_provider_preflight(&instance, &request,
          COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_ERROR_INVALID_ARG);
    CHECK(cog_provider_send(&instance, &request) == COG_ERROR_INVALID_ARG);
}
int main(void)
{
    test_required_preflight();
    test_out_param_preservation();
    test_call_counts_and_error_propagation();
    test_connect_failure_propagation();
    test_select_by_model_edge_cases();
    test_null_name_row();
    test_validation_boundaries();

    (void)printf("test_provider_critic: %d checks, %d failures\n",
                 g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
