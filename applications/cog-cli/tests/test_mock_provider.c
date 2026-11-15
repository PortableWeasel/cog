/**
 * @file test_mock_provider.c
 * @brief Contract test for the deterministic mock provider.
 *
 * Covers streaming chunks, error/retry metadata, cancellation (scripted
 * and one-shot), rate limits, scripted send failures, multi-turn repeat,
 * exhaustion, observable state, and disconnect semantics.
 */
#include "cog_cli/mock_provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
                          __LINE__, #cond);                             \
            ++failures;                                                 \
        }                                                               \
    } while (0)

#define CHECK_EQ_INT(a, b)                                              \
    do {                                                                \
        long long _a = (long long)(a);                                  \
        long long _b = (long long)(b);                                  \
        if (_a != _b) {                                                 \
            (void)fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n", \
                          __FILE__, __LINE__, #a, #b, _a, _b);          \
            ++failures;                                                 \
        }                                                               \
    } while (0)

#ifdef COG_MOCK_WRAP_ALLOC
static bool forbid_alloc;
/* Observe linker-wrapper effects across builtin/libc leaf assumptions. */
static volatile size_t allocations, fail_after;
void *__real_malloc(size_t n);
void *__real_calloc(size_t a, size_t b);
void *__real_realloc(void *p, size_t n);
void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t a, size_t b);
void *__wrap_realloc(void *p, size_t n);
void *__wrap_malloc(size_t n)
{
    ++allocations; CHECK(!forbid_alloc);
    if (fail_after != 0U && --fail_after == 0U) return NULL;
    return __real_malloc(n);
}
void *__wrap_calloc(size_t a, size_t b)
{
    ++allocations; CHECK(!forbid_alloc);
    if (fail_after != 0U && --fail_after == 0U) return NULL;
    return __real_calloc(a, b);
}
void *__wrap_realloc(void *p, size_t n)
{
    ++allocations; CHECK(!forbid_alloc);
    if (fail_after != 0U && --fail_after == 0U) return NULL;
    return __real_realloc(p, n);
}
/* Positive control: optimized direct allocator calls must reach wrappers,
 * update the real oracle, and honor its fault injection before use. */
static void allocation_oracle_control(void)
{
    size_t before = allocations;
    unsigned char *bytes = malloc(8U);
    assert(bytes != NULL && allocations == before + 1U);
    bytes[0] = 0x5aU;
    before = allocations;
    unsigned char *grown = realloc(bytes, 16U);
    assert(grown != NULL && allocations == before + 1U && grown[0] == 0x5aU);
    bytes = grown;
    free(bytes);
    before = allocations;
    bytes = calloc(8U, 1U);
    assert(bytes != NULL && allocations == before + 1U);
    assert(bytes[0] == 0U && bytes[7] == 0U);
    free(bytes);
    before = allocations;
    bytes = malloc(8U);
    free(bytes);
    assert(allocations == before + 1U); /* Detect optimized-away allocator pairs. */
    before = allocations;
    fail_after = 1U;
    bytes = malloc(8U);
    assert(bytes == NULL && allocations == before + 1U);
    assert(fail_after == 0U);
    fail_after = 0U;
    allocations = 0U;
}

static cog_result_t checked_preflight(cog_provider_t *p,
    const cog_provider_request_t *r, cog_provider_preflight_stage_t stage)
{
    size_t before = allocations;
    forbid_alloc = true;
    cog_result_t rc = cog_provider_preflight(p, r, stage);
    forbid_alloc = false;
    CHECK(allocations == before);
    return rc;
}
#define cog_provider_preflight checked_preflight
static void test_constructor_failures(void)
{
    for (size_t i = 1U; i <= 3U; ++i)
    {
        cog_provider_t *p = (cog_provider_t *)(uintptr_t)1U;
        fail_after = i;
        CHECK(cog_mock_provider_new(NULL, &p) == COG_ERROR_NOMEM && p == NULL);
        fail_after = 0U;
    }
}
#endif

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static cog_result_t do_send(cog_provider_t *p, const char *model)
{
    cog_provider_request_t req;
    cog_message_t msg = {0};

    (void)memset(&req, 0, sizeof(req));
    req.model_id = model;
    msg.role = COG_MESSAGE_ROLE_USER;
    msg.content = "hi";
    msg.content_len = 2U;
    msg.name = NULL;
    req.messages = &msg;
    req.message_count = 1U;
    req.stream = true;
    return cog_provider_send(p, &req);
}

static cog_result_t do_receive(cog_provider_t *p, char *buf, size_t cap,
                               size_t *out_len, bool *out_complete)
{
    cog_provider_response_t resp;

    (void)memset(&resp, 0, sizeof(resp));
    resp.content = buf;
    resp.content_capacity = cap;
    {
        cog_result_t rc = cog_provider_receive(p, &resp);

        if (rc == COG_OK) {
            if (out_len != NULL) {
                *out_len = resp.content_len;
            }
            if (out_complete != NULL) {
                *out_complete = resp.is_complete;
            }
        }
        return rc;
    }
}

/* ------------------------------------------------------------------ */
/* Streaming                                                           */
/* ------------------------------------------------------------------ */

static void test_stream_chunks(void)
{
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_CHUNK, "Hello", COG_OK, 0, false, 0U, NULL},
        {COG_MOCK_STEP_CHUNK, " world", COG_OK, 0, false, 0U, NULL},
    };
    static const cog_mock_script_t script = {
        steps, 2U, 0U, COG_OK, 0U,
    };
    cog_provider_t *p = NULL;
    cog_mock_state_t st;
    char buf[256];
    size_t len = 0U;
    bool complete = false;

    CHECK(cog_mock_provider_new(&script, &p) == COG_OK);
    CHECK(p != NULL);
    CHECK(do_send(p, "mock-model") == COG_OK);

    CHECK(do_receive(p, buf, sizeof(buf), &len, &complete) == COG_OK);
    CHECK(len == 5U && memcmp(buf, "Hello", 5U) == 0);
    CHECK(!complete);

    CHECK(do_receive(p, buf, sizeof(buf), &len, &complete) == COG_OK);
    CHECK(len == 6U && memcmp(buf, " world", 6U) == 0);
    CHECK(complete); /* last step of the pass */

    /* Exhausted: empty STOP completion. */
    CHECK(do_receive(p, buf, sizeof(buf), &len, &complete) == COG_ERROR_INVALID_STATE);

    CHECK(cog_mock_provider_state(p, &st) == COG_OK);
    CHECK_EQ_INT((long long)st.send_count, 1LL);
    CHECK_EQ_INT((long long)st.receive_count, 2LL);
    CHECK_EQ_INT((long long)st.chunk_count, 2LL);
    CHECK(st.last_request != NULL);
    CHECK(st.last_request->model_id != NULL &&
          strcmp(st.last_request->model_id, "mock-model") == 0);
    CHECK(st.last_request->stream);

    CHECK(cog_provider_disconnect(p) == COG_OK);
}

/* ------------------------------------------------------------------ */
/* Error / retry metadata                                              */
/* ------------------------------------------------------------------ */

static void test_error_retry_metadata(void)
{
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_ERROR, NULL, COG_ERROR_UNAVAILABLE, 429, true,
         5000U, "rate limited"},
        {COG_MOCK_STEP_CHUNK, "recovered", COG_OK, 0, false, 0U, NULL},
    };
    static const cog_mock_script_t script = {
        steps, 2U, 0U, COG_OK, 0U,
    };
    cog_provider_t *p = NULL;
    cog_mock_state_t st;
    char buf[256];
    size_t len = 0U;
    bool complete = false;

    CHECK(cog_mock_provider_new(&script, &p) == COG_OK);
    CHECK(do_send(p, "mock-model") == COG_OK);

    /* First receive fails with retry metadata. */
    CHECK(do_receive(p, buf, sizeof(buf), &len, &complete)
          == COG_ERROR_UNAVAILABLE);
    CHECK(cog_mock_provider_state(p, &st) == COG_OK);
    CHECK_EQ_INT((long long)st.error_count, 1LL);
    CHECK_EQ_INT(st.last_error.http_status, 429);
    CHECK(st.last_error.retryable);
    CHECK_EQ_INT((long long)st.last_error.retry_after_ms, 5000LL);
    CHECK(st.last_error.message != NULL &&
          strcmp(st.last_error.message, "mock turn failed") == 0);

    /* Retry (re-send) continues from the next step. */
    CHECK(do_send(p, "mock-model") == COG_OK);
    CHECK(do_receive(p, buf, sizeof(buf), &len, &complete) == COG_OK);
    CHECK(len == 9U && memcmp(buf, "recovered", 9U) == 0);
    CHECK(complete);

    CHECK(cog_provider_disconnect(p) == COG_OK);
}

/* ------------------------------------------------------------------ */
/* Cancellation                                                        */
/* ------------------------------------------------------------------ */

static void test_scripted_cancel(void)
{
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_CANCEL, NULL, COG_OK, 0, false, 0U, NULL},
    };
    static const cog_mock_script_t script = {
        steps, 1U, 0U, COG_OK, 0U,
    };
    cog_provider_t *p = NULL;
    cog_mock_state_t st;
    char buf[256];

    CHECK(cog_mock_provider_new(&script, &p) == COG_OK);
    CHECK(do_send(p, "mock-model") == COG_OK);
    CHECK(do_receive(p, buf, sizeof(buf), NULL, NULL) == COG_ERROR_ABORTED);
    CHECK(cog_mock_provider_state(p, &st) == COG_OK);
    CHECK_EQ_INT((long long)st.cancel_count, 1LL);
    CHECK(cog_provider_disconnect(p) == COG_OK);
}

static void test_oneshot_cancel(void)
{
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_CHUNK, "ok", COG_OK, 0, false, 0U, NULL},
    };
    static const cog_mock_script_t script = {
        steps, 1U, 0U, COG_OK, 0U,
    };
    cog_provider_t *p = NULL;
    cog_mock_state_t st;
    char buf[256];
    size_t len = 0U;
    bool complete = false;

    CHECK(cog_mock_provider_new(&script, &p) == COG_OK);
    CHECK(do_send(p, "mock-model") == COG_OK);

    /* Arm a one-shot cancel; the next receive aborts. */
    CHECK(cog_mock_provider_request_cancel(p) == COG_OK);
    CHECK(do_receive(p, buf, sizeof(buf), NULL, NULL) == COG_ERROR_ABORTED);
    CHECK(cog_mock_provider_state(p, &st) == COG_OK);
    CHECK_EQ_INT((long long)st.cancel_count, 1LL);
    CHECK(!st.cancel_requested); /* cleared after the abort */

    /* A later turn is not stuck cancelled. */
    CHECK(do_send(p, "mock-model") == COG_OK);
    CHECK(do_receive(p, buf, sizeof(buf), &len, &complete) == COG_OK);
    CHECK(len == 2U && memcmp(buf, "ok", 2U) == 0);

    CHECK(cog_provider_disconnect(p) == COG_OK);
}

/* ------------------------------------------------------------------ */
/* Send failure                                                        */
/* ------------------------------------------------------------------ */

static void test_send_error(void)
{
    static const cog_mock_script_t script = {
        NULL, 0U, 0U, COG_ERROR_IO, 0U,
    };
    cog_provider_t *p = NULL;

    CHECK(cog_mock_provider_new(&script, &p) == COG_OK);
    CHECK(do_send(p, "mock-model") == COG_ERROR_IO);
    CHECK(cog_provider_disconnect(p) == COG_OK);
}

/* ------------------------------------------------------------------ */
/* Multi-turn repeat                                                   */
/* ------------------------------------------------------------------ */

static void test_repeat(void)
{
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_CHUNK, "hi", COG_OK, 0, false, 0U, NULL},
    };
    static const cog_mock_script_t script = {
        steps, 1U, 2U, COG_OK, 0U, /* first pass + 2 replays = 3 passes */
    };
    cog_provider_t *p = NULL;
    char buf[256];
    size_t len = 0U;
    bool complete = false;
    int turn;

    CHECK(cog_mock_provider_new(&script, &p) == COG_OK);

    for (turn = 0; turn < 4; ++turn) {
        CHECK(do_send(p, "mock-model") == COG_OK);
        CHECK(do_receive(p, buf, sizeof(buf), &len, &complete) == COG_OK);
        if (turn < 3) {
            CHECK(len == 2U && memcmp(buf, "hi", 2U) == 0);
        } else {
            /* Repeats exhausted: empty completion. */
            CHECK(len == 0U);
        }
        CHECK(complete);
    }

    CHECK(cog_provider_disconnect(p) == COG_OK);
}

/* ------------------------------------------------------------------ */
/* Endpoint path + default script                                      */
/* ------------------------------------------------------------------ */

static void test_endpoint_connect(void)
{
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_CHUNK, "via endpoint", COG_OK, 0, false, 0U, NULL},
    };
    static const cog_mock_script_t script = {
        steps, 1U, 0U, COG_OK, 0U,
    };
    static const cog_endpoint_t ep = {
        .name = "mock",
        .vtable = &cog_mock_provider_vtable,
        .implementation_config = &script,
    };
    cog_provider_t *p = NULL;
    char buf[256];
    size_t len = 0U;
    bool complete = false;

    CHECK(cog_provider_connect(&ep, &p) == COG_OK);
    CHECK(p != NULL);
    CHECK(do_send(p, "mock-model") == COG_OK);
    CHECK(do_receive(p, buf, sizeof(buf), &len, &complete) == COG_OK);
    CHECK(len == 12U && memcmp(buf, "via endpoint", 12U) == 0);
    CHECK(cog_provider_disconnect(p) == COG_OK);

    /* Default endpoint (no script): one "mock response" chunk. */
    CHECK(cog_provider_connect(&cog_mock_endpoint, &p) == COG_OK);
    CHECK(do_send(p, "mock-model") == COG_OK);
    CHECK(do_receive(p, buf, sizeof(buf), &len, &complete) == COG_OK);
    CHECK(len == 13U && memcmp(buf, "mock response", 13U) == 0);
    CHECK(cog_provider_disconnect(p) == COG_OK);
}

/* ------------------------------------------------------------------ */
/* Disconnect semantics                                                */
/* ------------------------------------------------------------------ */

static void test_disconnect(void)
{
    cog_provider_t *p = NULL;

    CHECK(cog_mock_provider_new(NULL, &p) == COG_OK);
    CHECK(cog_provider_disconnect(p) == COG_OK);
    /* Disconnect on NULL is a no-op. */
    CHECK(cog_provider_disconnect(NULL) == COG_OK);
}

static void test_validation(void)
{
    static const cog_mock_script_t bad = {
        NULL, 1U, 0U, COG_OK, 0U, /* steps NULL but count 1 */
    };
    cog_provider_t *p = NULL;

    CHECK(cog_mock_provider_new(&bad, &p) == COG_ERROR_INVALID_ARG);
    CHECK(cog_mock_provider_new(NULL, NULL) == COG_ERROR_NULL_PTR);
}


static void test_typed_contract(void)
{
    static const unsigned char opaque[]="{\"reasoning_details\":[]}";
    static const cog_provider_delta_t deltas[]={
        {.kind=COG_PROVIDER_DELTA_TOOL_BEGIN,.tool_call_id="call_1",.tool_name="echo"},
        {.kind=COG_PROVIDER_DELTA_TOOL_ARGUMENTS,.tool_call_id="call_1",.data="{}",.data_len=2U},
        {.kind=COG_PROVIDER_DELTA_TOOL_END,.tool_call_id="call_1"},
        {.kind=COG_PROVIDER_DELTA_OPAQUE,.opaque={"openrouter.chat-completions","json-v1",opaque,sizeof(opaque)-1U}},
        {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_TOOL_CALLS,.outcome=COG_OK}
    };
    cog_provider_t *p=NULL;char storage[256];size_t required=0U;
    CHECK(cog_mock_provider_new_deltas(deltas,5U,&p)==COG_OK);
    CHECK(do_send(p,"mock-model")==COG_OK);
    cog_provider_delta_t d={0},before=d;
    CHECK(cog_provider_next(p,&d,storage,1U,&required)==COG_ERROR_OVERFLOW);
    CHECK(required==12U&&memcmp(&d,&before,sizeof(d))==0);
    uint64_t deadline;CHECK(cog_provider_poll(p,10U,&deadline)==COG_OK);
    for(size_t i=0U;i<5U;++i){
        CHECK(cog_provider_next(p,&d,storage,sizeof(storage),&required)==COG_OK);
        CHECK(d.kind==deltas[i].kind&&d.sequence==i+1U);
    }
    CHECK(cog_provider_next(p,&d,storage,sizeof(storage),&required)==COG_ERROR_INVALID_STATE);
    CHECK(cog_provider_disconnect(p)==COG_OK);
    CHECK(cog_mock_provider_new_deltas(deltas,5U,&p)==COG_OK);
    CHECK(do_send(p,"mock-model")==COG_OK);
    CHECK(cog_provider_next(p,&d,storage,1U,&required)==COG_ERROR_OVERFLOW);
    CHECK(cog_provider_cancel(p)==COG_OK);
    CHECK(cog_provider_next(p,&d,storage,sizeof(storage),&required)==COG_OK&&d.kind==COG_PROVIDER_DELTA_TOOL_BEGIN);
    CHECK(cog_provider_next(p,&d,storage,sizeof(storage),&required)==COG_OK&&d.kind==COG_PROVIDER_DELTA_FINISH&&d.outcome==COG_ERROR_ABORTED);
    CHECK(cog_provider_disconnect(p)==COG_OK);
}

static void test_poll_delay_and_request_copy(void)
{
    static const cog_mock_step_t steps[]={{COG_MOCK_STEP_CHUNK,"ready",COG_OK,0,false,0U,NULL}};
    static const cog_mock_script_t script={steps,1U,0U,COG_OK,10U};
    cog_provider_t *p=NULL;CHECK(cog_mock_provider_new(&script,&p)==COG_OK);
    char content[]="before",model[]="mock-model";
    cog_message_t message={.role=COG_MESSAGE_ROLE_USER,.content=content,.content_len=6U};
    cog_provider_request_t r={.model_id=model,.messages=&message,.message_count=1U};
    CHECK(cog_provider_send(p,&r)==COG_OK);memset(content,'X',6U);memset(model,'X',10U);
    cog_mock_state_t state;CHECK(cog_mock_provider_state(p,&state)==COG_OK);
    CHECK(strcmp(state.last_request->model_id,"mock-model")==0);
    CHECK(strcmp(state.last_request->messages[0].content,"before")==0);
    cog_provider_delta_t d;char storage[32];size_t required;uint64_t deadline;
    CHECK(cog_provider_next(p,&d,storage,sizeof(storage),&required)==COG_ERROR_AGAIN);
    CHECK(cog_provider_poll(p,9U,&deadline)==COG_OK&&deadline==10U);
    CHECK(cog_provider_next(p,&d,storage,sizeof(storage),&required)==COG_ERROR_AGAIN);
    CHECK(cog_provider_poll(p,10U,&deadline)==COG_OK);
    CHECK(cog_provider_next(p,&d,storage,sizeof(storage),&required)==COG_OK&&strcmp(d.data,"ready")==0);
    CHECK(cog_provider_next(p,&d,storage,sizeof(storage),&required)==COG_OK&&d.kind==COG_PROVIDER_DELTA_FINISH);
    CHECK(cog_provider_disconnect(p)==COG_OK);
}

/* Retrying retained legacy text must not hide the cancellation terminal. */
static void test_receive_overflow_then_cancel(void)
{
    static const cog_mock_step_t steps[] = {
        {COG_MOCK_STEP_CHUNK, "retained", COG_OK, 0, false, 0U, NULL},
        {COG_MOCK_STEP_CHUNK, "unconsumed", COG_OK, 0, false, 0U, NULL},
    };
    static const cog_mock_script_t script = {steps, 2U, 0U, COG_OK, 0U};
    cog_provider_t *p = NULL;
    char bytes[32];
    cog_provider_response_t response = {.content = bytes,
                                        .content_capacity = 1U};
    CHECK(cog_mock_provider_new(&script, &p) == COG_OK);
    CHECK(do_send(p, "mock-model") == COG_OK);
    CHECK(cog_provider_receive(p, &response) == COG_ERROR_OVERFLOW);
    CHECK(cog_provider_cancel(p) == COG_OK);
    CHECK(cog_provider_cancel(p) == COG_OK);
    response.content_capacity = sizeof(bytes);
    CHECK(cog_provider_receive(p, &response) == COG_OK);
    CHECK(strcmp(bytes, "retained") == 0);
    CHECK(!response.is_complete);
    CHECK(response.finish_reason == COG_FINISH_REASON_NONE);
    CHECK(cog_provider_receive(p, &response) == COG_ERROR_ABORTED);
    CHECK(response.is_complete);
    CHECK(response.finish_reason == COG_FINISH_REASON_CANCELLED);
    CHECK(cog_provider_receive(p, &response) == COG_ERROR_INVALID_STATE);
    CHECK(cog_provider_disconnect(p) == COG_OK);
}

static void test_typed_cancel_error_after_retained_output(void)
{
    static const cog_provider_delta_t script[] = {
        {.kind = COG_PROVIDER_DELTA_TEXT, .data = "retained", .data_len = 8U},
        {.kind = COG_PROVIDER_DELTA_FINISH, .finish_reason = COG_FINISH_REASON_STOP},
    };
    cog_provider_t *p = NULL;
    char bytes[32];
    size_t required;
    cog_provider_delta_t delta;
    cog_provider_error_t error;
    CHECK(cog_mock_provider_new_deltas(script, 2U, &p) == COG_OK);
    CHECK(do_send(p, "mock-model") == COG_OK);
    CHECK(cog_provider_next(p, &delta, NULL, 0U, &required) == COG_ERROR_OVERFLOW);
    CHECK(cog_provider_cancel(p) == COG_OK);
    CHECK(cog_provider_get_error(p, &error) == COG_OK);
    CHECK(error.output_accepted && !error.replay_safe);
    CHECK(cog_provider_next(p, &delta, bytes, sizeof(bytes), &required) == COG_OK);
    CHECK(delta.kind == COG_PROVIDER_DELTA_TEXT);
    CHECK(cog_provider_next(p, &delta, bytes, sizeof(bytes), &required) == COG_OK);
    CHECK(delta.kind == COG_PROVIDER_DELTA_FINISH && delta.outcome == COG_ERROR_ABORTED);
    CHECK(cog_provider_get_error(p, &error) == COG_OK);
    CHECK(error.output_accepted);
    CHECK(!error.replay_safe);
    CHECK(cog_provider_disconnect(p) == COG_OK);
}

static void test_nested_request_copy(void)
{
    char model[] = "mock-model", tools[] = "[]", content[] = "assistant";
    char name[] = "named", id[] = "call_1", function[] = "echo";
    char arguments[] = "{}", ns[] = "example", format[] = "binary", currency[] = "USD";
    unsigned char data[] = {0U, 255U, 1U};
    cog_message_tool_call_t call = {id, function, arguments, 2U};
    cog_message_opaque_t opaque = {ns, format, data, sizeof(data)};
    cog_message_t message = {.role = COG_MESSAGE_ROLE_ASSISTANT,
                             .content = content, .content_len = 9U,
                             .name = name, .tool_calls = &call, .tool_call_count = 1U,
                             .opaque = &opaque, .opaque_count = 1U,
                             .usage = {.currency = currency}};
    cog_message_t history[] = {message, {.role = COG_MESSAGE_ROLE_TOOL,
                                         .tool_call_id = id}};
    cog_provider_request_t request = {.model_id = model, .messages = history,
                                      .message_count = 2U, .tools_json = tools,
                                      .tools_json_len = 2U};
    cog_provider_t *p = NULL;
    CHECK(cog_mock_provider_new(NULL, &p) == COG_OK);
    CHECK(cog_provider_send(p, &request) == COG_OK);
    memset(model, 'X', sizeof(model)); memset(tools, 'X', sizeof(tools));
    memset(content, 'X', sizeof(content)); memset(name, 'X', sizeof(name));
    memset(id, 'X', sizeof(id)); memset(function, 'X', sizeof(function));
    memset(arguments, 'X', sizeof(arguments)); memset(ns, 'X', sizeof(ns));
    memset(format, 'X', sizeof(format)); memset(currency, 'X', sizeof(currency));
    memset(data, 0, sizeof(data)); memset(&call, 0, sizeof(call));
    memset(&opaque, 0, sizeof(opaque)); memset(&message, 0, sizeof(message));
    memset(&request, 0, sizeof(request));
    cog_mock_state_t state;
    CHECK(cog_mock_provider_state(p, &state) == COG_OK);
    const cog_provider_request_t *saved = state.last_request;
    CHECK(strcmp(saved->model_id, "mock-model") == 0);
    CHECK(strcmp(saved->tools_json, "[]") == 0);
    const cog_message_t *m = &saved->messages[0];
    CHECK(strcmp(m->content, "assistant") == 0 && strcmp(m->name, "named") == 0);
    CHECK(strcmp(m->tool_calls[0].id, "call_1") == 0);
    CHECK(strcmp(m->tool_calls[0].name, "echo") == 0);
    CHECK(strcmp(m->tool_calls[0].arguments, "{}") == 0);
    CHECK(strcmp(m->opaque[0].namespace_id, "example") == 0);
    CHECK(strcmp(m->opaque[0].format, "binary") == 0);
    CHECK(m->opaque[0].size == 3U && m->opaque[0].data[1] == 255U);
    CHECK(strcmp(m->usage.currency, "USD") == 0);
    CHECK(cog_provider_disconnect(p) == COG_OK);
}
static void test_preflight_history(void)
{
    cog_provider_t *p = NULL;
    cog_message_tool_call_t calls[] = {{"a", "one", "{}", 2U},
                                      {"b", "two", "{}", 2U},
                                      {"c", "three", "{}", 2U}};
    cog_message_t messages[7] = {
        {.role = COG_MESSAGE_ROLE_ASSISTANT, .tool_calls = calls,
         .tool_call_count = 3U}};
    cog_provider_request_t r = {.model_id = "any-fixture", .messages = messages};
    CHECK(cog_mock_provider_new(NULL, &p) == COG_OK);
    CHECK(cog_provider_preflight(NULL, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_ERROR_NULL_PTR);
    CHECK(cog_provider_preflight(p, NULL, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_ERROR_NULL_PTR);
    CHECK(cog_provider_preflight(p, &r, (cog_provider_preflight_stage_t)99) == COG_ERROR_INVALID_ARG);
    for (unsigned mask = 0U; mask < 8U; ++mask)
        for (unsigned reverse = 0U; reverse < 2U; ++reverse)
        {
            r.message_count = 1U;
            for (unsigned j = 0U; j < 3U; ++j)
            {
                unsigned k = reverse != 0U ? 2U - j : j;
                if ((mask & (1U << k)) != 0U)
                    messages[r.message_count++] = (cog_message_t){
                        .role = COG_MESSAGE_ROLE_TOOL, .tool_call_id = calls[k].id,
                        .name = "a descriptive result name"};
            }
            CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) ==
                  (mask == 7U ? COG_OK : COG_ERROR_INVALID_STATE));
            CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_PENDING_TOOLS) ==
                  (mask == 7U ? COG_ERROR_INVALID_STATE : COG_OK));
            if (mask != 7U)
                CHECK(cog_provider_send(p, &r) == COG_ERROR_INVALID_STATE);
        }
    /* Closed groups can reuse an ID; a pending group cannot be crossed. */
    messages[0].tool_call_count = 1U;
    messages[1] = (cog_message_t){.role = COG_MESSAGE_ROLE_TOOL, .tool_call_id = "a"};
    messages[2] = messages[0];
    r.message_count = 3U;
    CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_PENDING_TOOLS) == COG_OK);
    messages[3] = messages[1]; r.message_count = 4U;
    CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_OK);
    messages[3].tool_call_id = "b";
    CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_ERROR_INVALID_STATE);
    messages[3] = (cog_message_t){.role = COG_MESSAGE_ROLE_USER};
    CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_PENDING_TOOLS) == COG_ERROR_INVALID_STATE);
    r.message_count = 2U; messages[1].tool_call_id = "unknown";
    CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_ERROR_INVALID_STATE);
    r.messages = &messages[1]; r.message_count = 1U;
    CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_ERROR_INVALID_STATE);
    r.messages = messages; r.message_count = 3U;
    messages[1].tool_call_id = "a"; messages[2] = messages[1];
    CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_ERROR_INVALID_STATE);
    r.message_count = 1U;
    for (int status = (int)COG_MESSAGE_INCOMPLETE; status <= (int)COG_MESSAGE_FAILED; ++status)
    {
        messages[0].status = (cog_message_status_t)status;
        CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_PENDING_TOOLS) == COG_ERROR_INVALID_STATE);
    }
    messages[0].status = COG_MESSAGE_COMPLETE;
    calls[0].name = "\xc0\xaf";
    CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_PENDING_TOOLS) == COG_ERROR_INVALID_ARG);
    calls[0].name = "one";
    messages[0].tool_call_count = 0U;
    CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_PENDING_TOOLS) == COG_ERROR_INVALID_STATE);
    CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_OK);
    cog_mock_state_t state;
    CHECK(cog_mock_provider_state(p, &state) == COG_OK);
    CHECK(state.send_count == 0U && state.receive_count == 0U && state.step_index == 0U);
    CHECK(cog_provider_disconnect(p) == COG_OK);
}
static void test_preflight_fixture_state(void)
{
    cog_provider_t *p = NULL;
    CHECK(cog_mock_provider_new(NULL, &p) == COG_OK);
    CHECK(do_send(p, "retained-model") == COG_OK);
    cog_provider_request_t r = {.model_id = "next-model", .configuration_generation = 99U};
    char bytes[64]; size_t required;
    cog_provider_delta_t delta;
    CHECK(cog_provider_next(p, &delta, NULL, 0U, &required) == COG_ERROR_OVERFLOW);
    CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_ERROR_INVALID_STATE);
    CHECK(cog_provider_cancel(p) == COG_OK);
    CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_ERROR_INVALID_STATE);
    CHECK(cog_provider_next(p, &delta, bytes, sizeof(bytes), &required) == COG_OK);
    CHECK(delta.kind == COG_PROVIDER_DELTA_TEXT && delta.sequence == 1U &&
          strcmp(delta.data, "mock response") == 0);
    CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_ERROR_INVALID_STATE);
    CHECK(cog_provider_next(p, &delta, bytes, sizeof(bytes), &required) == COG_OK);
    CHECK(delta.kind == COG_PROVIDER_DELTA_FINISH && delta.outcome == COG_ERROR_ABORTED);
    cog_mock_state_t before, after;
    cog_provider_error_t old, next;
    CHECK(cog_mock_provider_state(p, &before) == COG_OK);
    CHECK(cog_provider_get_error(p, &old) == COG_OK);
    const char *saved_model = before.last_request->model_id;
    unsigned char arbitrary[] = {0U, 255U, 1U};
    cog_message_opaque_t o = {"any-provider", "uninterpreted", arbitrary, sizeof(arbitrary)};
    cog_message_t m = {.role = COG_MESSAGE_ROLE_ASSISTANT, .opaque = &o, .opaque_count = 1U};
    r.messages = &m; r.message_count = 1U;
    CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_OK);
    char *large = malloc(131072U);
    CHECK(large != NULL);
    if (large != NULL)
    {
        memset(large, 'x', 131072U);
        m.content = large; m.content_len = 131072U;
        CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_ERROR_LIMIT);
        /* Locate the exact aligned copy boundary, then test both sides and
         * confirm COMPLETE send uses that same representation ceiling. */
        size_t low = 0U, high = 131072U;
        while (low + 1U < high)
        {
            size_t mid = low + (high - low) / 2U;
            m.content_len = mid;
            cog_result_t rc = cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_COMPLETE);
            CHECK(rc == COG_OK || rc == COG_ERROR_LIMIT);
            if (rc == COG_OK) low = mid; else high = mid;
        }
        m.content_len = low;
        CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_OK);
        cog_provider_t *boundary = NULL;
        CHECK(cog_mock_provider_new(NULL, &boundary) == COG_OK);
        CHECK(cog_provider_send(boundary, &r) == COG_OK);
        CHECK(cog_provider_disconnect(boundary) == COG_OK);
        m.content_len = high;
        CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_ERROR_LIMIT);
        CHECK(cog_provider_send(p, &r) == COG_ERROR_LIMIT);
        free(large);
    }
    CHECK(cog_mock_provider_state(p, &after) == COG_OK);
    CHECK(after.last_request == before.last_request && after.last_request->model_id == saved_model);
    CHECK(strcmp(saved_model, "retained-model") == 0);
    CHECK(after.send_count == before.send_count && after.receive_count == before.receive_count &&
          after.chunk_count == before.chunk_count && after.cancel_count == before.cancel_count &&
          after.error_count == before.error_count && after.step_index == before.step_index &&
          after.cancel_requested == before.cancel_requested);
    CHECK(cog_provider_get_error(p, &next) == COG_OK);
    CHECK(next.code == old.code && next.message == old.message && next.kind == old.kind &&
          next.output_accepted == old.output_accepted && next.replay_safe == old.replay_safe);
    uint64_t deadline;
    CHECK(cog_provider_poll(p, 0U, &deadline) == COG_OK);
    CHECK(cog_provider_disconnect(p) == COG_OK);
    static const cog_mock_script_t failing = {.send_error = COG_ERROR_UNAVAILABLE};
    CHECK(cog_mock_provider_new(&failing, &p) == COG_OK);
    r.messages = NULL; r.message_count = 0U;
    CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_OK);
    CHECK(cog_provider_send(p, &r) == COG_ERROR_UNAVAILABLE);
    CHECK(cog_provider_preflight(p, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_OK);
    CHECK(cog_provider_get_error(p, &next) == COG_ERROR_NOT_FOUND);
    CHECK(cog_provider_disconnect(p) == COG_OK);
}
int main(void)
{
#ifdef COG_MOCK_WRAP_ALLOC
    allocation_oracle_control();
    test_constructor_failures();
#endif
    test_preflight_history();
    test_preflight_fixture_state();
    test_nested_request_copy();
    test_typed_cancel_error_after_retained_output();
    test_receive_overflow_then_cancel();
    test_poll_delay_and_request_copy();
    test_typed_contract();
    test_stream_chunks();
    test_error_retry_metadata();
    test_scripted_cancel();
    test_oneshot_cancel();
    test_send_error();
    test_repeat();
    test_endpoint_connect();
    test_disconnect();
    test_validation();

    if (failures != 0) {
        (void)fprintf(stderr, "test_mock_provider: %d failure(s)\n",
                      failures);
        return 1;
    }
    (void)printf("test_mock_provider: all checks passed\n");
    return 0;
}
