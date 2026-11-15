/** @file test_message.c @brief Structural message validation contract tests. */
#include "cog/message.h"
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define CHECK(expr) do { if (!(expr)) { \
    (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
    abort(); } } while (0)


int main(void)
{
    cog_message_t m = {0};
    CHECK(cog_message_validate(NULL) == COG_ERROR_NULL_PTR);
    CHECK(cog_message_validate(&m) == COG_OK);
    m.role = (cog_message_role_t)9;
    CHECK(cog_message_validate(&m) == COG_ERROR_INVALID_ARG);
    m.role = COG_MESSAGE_ROLE_USER;
    m.content_len = 1U;
    CHECK(cog_message_validate(&m) == COG_ERROR_INVALID_ARG);
    m.content = "a";
    CHECK(cog_message_validate(&m) == COG_OK);
    static const char *const invalid[] = {"\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80", "a\0b"};
    static const size_t lengths[] = {2U, 3U, 4U, 2U, 1U, 3U};
    for (size_t i = 0U; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        m.content = invalid[i]; m.content_len = lengths[i];
        CHECK(cog_message_validate(&m) == COG_ERROR_INVALID_ARG);
    }
    m.content = "\xc2\xa2\xe2\x82\xac\xf0\x90\x8d\x88"; m.content_len = 9U;
    CHECK(cog_message_validate(&m) == COG_OK);
    cog_message_tool_call_t calls[] = {{"one", "read", "{}", 2U}, {"two", "read", "{", 1U}};
    m.tool_calls = calls; m.tool_call_count = 2U;
    CHECK(cog_message_validate(&m) == COG_ERROR_INVALID_ARG);
    m.role = COG_MESSAGE_ROLE_ASSISTANT;
    /* Syntax is deliberately the consuming provider/toolkit's responsibility. */
    CHECK(cog_message_validate(&m) == COG_OK);
    calls[1].id = "one";
    CHECK(cog_message_validate(&m) == COG_ERROR_INVALID_ARG);
    calls[1].id = "";
    CHECK(cog_message_validate(&m) == COG_ERROR_INVALID_ARG);
    calls[1].id = "two";
    calls[1].name = NULL;
    CHECK(cog_message_validate(&m) == COG_ERROR_INVALID_ARG);
    calls[1].name = "read";
    m.tool_call_id = "wrong";
    CHECK(cog_message_validate(&m) == COG_ERROR_INVALID_ARG);
    m.tool_call_id = NULL;
    const unsigned char bytes[] = {0U, 1U, 255U};
    cog_message_opaque_t opaque = {"provider", "format", bytes, sizeof(bytes)};
    m.opaque = &opaque; m.opaque_count = 1U;
    CHECK(cog_message_validate(&m) == COG_OK);
    opaque.data = NULL;
    CHECK(cog_message_validate(&m) == COG_ERROR_INVALID_ARG);
    opaque.data = bytes;
    m.usage.cost_known = true; m.usage.cost = 0.0;
    CHECK(cog_message_validate(&m) == COG_ERROR_INVALID_ARG);
    m.usage.currency = "USD";
    CHECK(cog_message_validate(&m) == COG_OK);
    m.usage.cost = -1.0;
    CHECK(cog_message_validate(&m) == COG_ERROR_INVALID_ARG);
    m.usage.cost = NAN;
    CHECK(cog_message_validate(&m) == COG_ERROR_INVALID_ARG);
    m.usage.cost = INFINITY;
    CHECK(cog_message_validate(&m) == COG_ERROR_INVALID_ARG);
    m = (cog_message_t){.role = COG_MESSAGE_ROLE_TOOL};
    CHECK(cog_message_validate(&m) == COG_ERROR_INVALID_ARG);
    m.tool_call_id = "one";
    CHECK(cog_message_validate(&m) == COG_OK);
    m.opaque_count = 1U; m.opaque = &opaque;
    CHECK(cog_message_validate(&m) == COG_ERROR_INVALID_ARG);
    puts("message structural contracts passed");
    return 0;
}
