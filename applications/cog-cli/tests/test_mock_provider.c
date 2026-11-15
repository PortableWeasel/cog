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
int main(void)
{
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
