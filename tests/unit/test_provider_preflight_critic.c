/** @file test_provider_preflight_critic.c @brief Independent staged-history model. */
#include <cog/provider.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
    cog_provider_t base;
    size_t validations, accepted;
    uint64_t clock;
    bool active;
    cog_result_t validation_result;
    cog_provider_error_t prior_error;
} fixture_t;
static cog_result_t connect_fixture(const cog_endpoint_t *ep, cog_provider_t **out)
{ (void)ep; *out = NULL; return COG_ERROR_UNSUPPORTED; }
static cog_result_t disconnect_fixture(cog_provider_t *p)
{ (void)p; return COG_OK; }
static cog_result_t send_fixture(cog_provider_t *p, const cog_provider_request_t *r)
{ fixture_t *f = (fixture_t *)p; (void)r; ++f->accepted; return COG_OK; }
static cog_result_t receive_fixture(cog_provider_t *p, cog_provider_response_t *r)
{ (void)p; (void)r; return COG_ERROR_AGAIN; }
static cog_result_t poll_fixture(cog_provider_t *p, uint64_t now, uint64_t *deadline)
{ (void)p; (void)now; *deadline = UINT64_MAX; return COG_OK; }
static cog_result_t cancel_fixture(cog_provider_t *p)
{ (void)p; return COG_OK; }
static cog_result_t next_fixture(cog_provider_t *p, cog_provider_delta_t *d,
                                 void *bytes, size_t cap, size_t *required)
{ (void)p; (void)d; (void)bytes; (void)cap; *required = 0; return COG_ERROR_AGAIN; }
static cog_result_t error_fixture(const cog_provider_t *p, cog_provider_error_t *out)
{ *out = ((const fixture_t *)p)->prior_error; return COG_OK; }
static cog_result_t preflight_fixture(cog_provider_t *p,
    const cog_provider_request_t *r, cog_provider_preflight_stage_t stage)
{
    fixture_t *f = (fixture_t *)p;
    (void)r; (void)stage;
    ++f->validations;
    return f->active ? COG_ERROR_INVALID_STATE : f->validation_result;
}
static const cog_provider_vtable_t fixture_ops = {
    connect_fixture, disconnect_fixture, send_fixture, receive_fixture,
    poll_fixture, cancel_fixture, next_fixture, error_fixture, preflight_fixture};
static const cog_message_tool_call_t calls[] = {{"a", "echo", "{}", 2}, {"b", "echo", "{}", 2}};

/* A separate finite-state model tracks the unresolved IDs as two bits. It has
 * no group pointer, result lookup or backward duplicate scan like production. */
static bool reference(const unsigned char *tokens, size_t count, bool pending)
{
    unsigned remaining = 0;
    for (size_t i = 0; i < count; ++i) {
        unsigned char token = tokens[i];
        if (token >= 4) {
            unsigned bit = token == 4 ? 1U : 2U;
            if ((remaining & bit) == 0) return false;
            remaining &= ~bit;
        } else {
            if (remaining != 0) return false;
            remaining = token == 2 ? 1U : token == 3 ? 3U : 0U;
        }
    }
    return pending == (remaining != 0);
}
static cog_message_t message(unsigned char token)
{
    cog_message_t m = {0};
    if (token == 0) { m.role = COG_MESSAGE_ROLE_USER; m.content = "user"; m.content_len = 4; }
    else if (token < 4) {
        m.role = COG_MESSAGE_ROLE_ASSISTANT;
        if (token > 1) { m.tool_calls = calls; m.tool_call_count = token == 2 ? 1U : 2U; }
    } else { m.role = COG_MESSAGE_ROLE_TOOL; m.tool_call_id = token == 4 ? "a" : "b"; m.name = "descriptive"; }
    return m;
}
static void check_error(const fixture_t *f)
{
    cog_provider_error_t e;
    assert(cog_provider_get_error(&f->base, &e) == COG_OK);
    assert(e.code == COG_ERROR_UNAVAILABLE && e.http_status == 429 && e.retry_after_ms == 900);
    assert(e.message == f->prior_error.message && strcmp(e.message, "retained terminal") == 0);
    assert(e.retryable && !e.output_accepted && e.replay_safe && f->clock == 123);
}
int main(void)
{
    fixture_t f = {.base = {&fixture_ops}, .clock = 123,
        .prior_error = {.code = COG_ERROR_UNAVAILABLE, .http_status = 429,
            .message = "retained terminal", .retryable = true, .retry_after_ms = 900,
            .replay_safe = true}};
    cog_message_t history[6];
    unsigned char tokens[6];
    size_t words = 1, cases = 0;
    for (size_t count = 0; count <= 6; ++count) {
        for (size_t word = 0; word < words; ++word) {
            size_t rest = word;
            for (size_t i = 0; i < count; ++i) {
                tokens[i] = (unsigned char)(rest % 6U); rest /= 6U;
                history[i] = message(tokens[i]);
            }
            cog_provider_request_t r = {.model_id = "fixture", .messages = history, .message_count = count};
            for (unsigned stage = 0; stage < 2; ++stage) {
                bool valid = reference(tokens, count, stage != 0);
                size_t validations = f.validations, accepted = f.accepted;
                assert(cog_provider_preflight(&f.base, &r, (cog_provider_preflight_stage_t)stage) ==
                       (valid ? COG_OK : COG_ERROR_INVALID_STATE));
                assert(f.validations == validations + (valid ? 1U : 0U) && f.accepted == accepted);
                check_error(&f);
            }
            size_t accepted = f.accepted;
            bool complete = reference(tokens, count, false);
            assert(cog_provider_send(&f.base, &r) == (complete ? COG_OK : COG_ERROR_INVALID_STATE));
            assert(f.accepted == accepted + (complete ? 1U : 0U));
            ++cases;
        }
        words *= 6U;
    }
    assert(cases == 55987U);
    cog_provider_request_t r = {.model_id = "fixture", .messages = history, .message_count = 1};
    history[0] = message(0);
    static const cog_result_t failures[] = {COG_ERROR_LIMIT, COG_ERROR_UNSUPPORTED, COG_ERROR_PROTOCOL, COG_ERROR_NOT_FOUND};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        size_t accepted = f.accepted;
        f.validation_result = failures[i];
        assert(cog_provider_preflight(&f.base, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == failures[i]);
        assert(cog_provider_send(&f.base, &r) == failures[i] && f.accepted == accepted);
        check_error(&f);
    }
    f.validation_result = COG_OK; f.active = true;
    assert(cog_provider_preflight(&f.base, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_ERROR_INVALID_STATE);
    check_error(&f);
    f.active = false;
    for (int status = COG_MESSAGE_INCOMPLETE; status <= COG_MESSAGE_FAILED; ++status) {
        history[0].status = (cog_message_status_t)status;
        assert(cog_provider_preflight(&f.base, &r, COG_PROVIDER_PREFLIGHT_COMPLETE) == COG_ERROR_INVALID_STATE);
    }
    puts("independent preflight model: 55,987 histories, staged/send separation and failure-state checks passed");
    return 0;
}
