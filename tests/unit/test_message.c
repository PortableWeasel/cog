/**
 * @file test_message.c
 * @brief Phase 0 contract test for cog/message.h (header-only module).
 *
 * message.h declares no functions; it defines the cog_message_role_t
 * enumeration and the cog_message_t struct.  This test pins the role
 * values, the struct shape and field types, and the ownership and
 * content conventions documented in the header:
 *
 *   - content is referenced, not owned, and is length-prefixed
 *     (content_len in bytes), so embedded NUL bytes are payload;
 *   - content may be NULL when content_len is zero — in particular a
 *     zero-initialized message is a valid empty message;
 *   - COG_MESSAGE_ROLE_TOOL uses name to identify the tool call;
 *     name may be NULL for the other roles;
 *   - messages are read-only after construction (const pointers to
 *     struct and content compile without casts).
 *
 * Nothing is tested here that is not either stated in the header or
 * guaranteed by C11 semantics.
 *
 * Ticket: 9d5b705 (Stage 12.0 — Foundation: result + message)
 */

#include "cog/message.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifndef COG_MESSAGE_H
#error "COG_MESSAGE_H include guard missing"
#endif

/* ------------------------------------------------------------------ */
/* Compile-time checks: role values and struct shape.                  */
/* ------------------------------------------------------------------ */

_Static_assert(COG_MESSAGE_ROLE_SYSTEM == 0,
               "COG_MESSAGE_ROLE_SYSTEM must be exactly 0");
_Static_assert(COG_MESSAGE_ROLE_USER == 1,
               "COG_MESSAGE_ROLE_USER must be exactly 1");
_Static_assert(COG_MESSAGE_ROLE_ASSISTANT == 2,
               "COG_MESSAGE_ROLE_ASSISTANT must be exactly 2");
_Static_assert(COG_MESSAGE_ROLE_TOOL == 3,
               "COG_MESSAGE_ROLE_TOOL must be exactly 3");

/* Field types: exact, verified via type-generic selection (unevaluated). */
#define FIELD_TYPE_ASSERT(field, type)                                       \
    _Static_assert(_Generic(((cog_message_t){0}).field, type : 1, default : 0), \
                   "cog_message_t." #field " must have type " #type)

FIELD_TYPE_ASSERT(role, cog_message_role_t);
FIELD_TYPE_ASSERT(content, const char *);
FIELD_TYPE_ASSERT(content_len, size_t);
FIELD_TYPE_ASSERT(name, const char *);

#undef FIELD_TYPE_ASSERT

/* ------------------------------------------------------------------ */
/* Runtime checks.                                                     */
/* ------------------------------------------------------------------ */

/** All four roles must be distinct and cover the range 0..3 in order. */
static int verify_role_values(void)
{
    static const cog_message_role_t roles[] = {
        COG_MESSAGE_ROLE_SYSTEM,
        COG_MESSAGE_ROLE_USER,
        COG_MESSAGE_ROLE_ASSISTANT,
        COG_MESSAGE_ROLE_TOOL,
    };
    const size_t n = sizeof(roles) / sizeof(roles[0]);
    size_t i;

    if (n != 4U) {
        return 1;
    }
    for (i = 0U; i < n; ++i) {
        if ((int)roles[i] != (int)i) {
            (void)fprintf(stderr, "role[%zu] has unexpected value %d\n",
                          i, (int)roles[i]);
            return 1;
        }
    }
    return 0;
}

/** Roles must be usable in a switch (as provider/tool dispatch does). */
static int role_index(cog_message_role_t role)
{
    switch (role) {
    case COG_MESSAGE_ROLE_SYSTEM:    return 0;
    case COG_MESSAGE_ROLE_USER:      return 1;
    case COG_MESSAGE_ROLE_ASSISTANT: return 2;
    case COG_MESSAGE_ROLE_TOOL:      return 3;
    default:                         return -1;
    }
}

static int verify_role_switch(void)
{
    static const cog_message_role_t roles[] = {
        COG_MESSAGE_ROLE_SYSTEM,
        COG_MESSAGE_ROLE_USER,
        COG_MESSAGE_ROLE_ASSISTANT,
        COG_MESSAGE_ROLE_TOOL,
    };
    const size_t n = sizeof(roles) / sizeof(roles[0]);
    size_t i;

    for (i = 0U; i < n; ++i) {
        if (role_index(roles[i]) != (int)i) {
            return 1;
        }
    }
    return 0;
}

/**
 * A zero-initialized message is a valid empty message: role is
 * COG_MESSAGE_ROLE_SYSTEM, content is NULL, content_len is 0, and
 * name is NULL.  This is exactly the "content may be NULL when
 * content_len is zero" case.
 */
static int verify_zero_initialized_message(void)
{
    const cog_message_t m = {0};

    if (m.role != COG_MESSAGE_ROLE_SYSTEM) {
        return 1;
    }
    if (m.content != NULL) {
        return 1;
    }
    if (m.content_len != 0U) {
        return 1;
    }
    if (m.name != NULL) {
        return 1;
    }
    return 0;
}

/**
 * Content is length-prefixed in bytes: embedded NUL bytes are payload,
 * not terminators, and an explicitly zero-length message is valid.
 */
static int verify_length_prefixed_content(void)
{
    static const char binary_payload[] = {'a', '\0', '\0', 'b'};
    const cog_message_t binary = {
        .role = COG_MESSAGE_ROLE_USER,
        .content = binary_payload,
        .content_len = sizeof(binary_payload),
        .name = NULL,
    };
    const cog_message_t empty = {
        .role = COG_MESSAGE_ROLE_USER,
        .content = NULL,
        .content_len = 0U,
        .name = NULL,
    };

    if (binary.content_len != 4U) {
        return 1;
    }
    if (binary.content[0] != 'a' || binary.content[1] != '\0' ||
        binary.content[2] != '\0' || binary.content[3] != 'b') {
        return 1;
    }
    if (empty.content_len != 0U || empty.content != NULL) {
        return 1;
    }
    return 0;
}

/**
 * COG_MESSAGE_ROLE_TOOL uses name to identify the tool call; name may
 * be NULL for the other roles.  Content references string literals
 * (referenced, not owned) without needing casts — const-qualified.
 */
static int verify_tool_role_uses_name(void)
{
    static const char tool_content[] = "{\"city\": \"Kyoto\", \"temp_c\": 18}";
    const cog_message_t tool = {
        .role = COG_MESSAGE_ROLE_TOOL,
        .content = tool_content,
        .content_len = sizeof(tool_content) - 1U,
        .name = "get_weather",
    };
    const cog_message_t assistant = {
        .role = COG_MESSAGE_ROLE_ASSISTANT,
        .content = "I can help with that.",
        .content_len = sizeof("I can help with that.") - 1U,
        .name = NULL,
    };
    const char *content_view = tool.content;   /* no cast: content is const */
    const char *name_view = tool.name;          /* no cast: name is const */

    if (tool.role != COG_MESSAGE_ROLE_TOOL || tool.name == NULL) {
        return 1;
    }
    if (strcmp(tool.name, "get_weather") != 0) {
        return 1;
    }
    if (assistant.role != COG_MESSAGE_ROLE_ASSISTANT || assistant.name != NULL) {
        return 1;
    }
    if (content_view[0] != '{' || name_view[0] != 'g') {
        return 1;
    }
    return 0;
}

/** The struct must have four distinct members (no aliasing layout bug). */
static int verify_member_layout(void)
{
    const size_t offs[] = {
        offsetof(cog_message_t, role),
        offsetof(cog_message_t, content),
        offsetof(cog_message_t, content_len),
        offsetof(cog_message_t, name),
    };
    const size_t n = sizeof(offs) / sizeof(offs[0]);
    size_t i;
    size_t j;

    for (i = 0U; i < n; ++i) {
        for (j = i + 1U; j < n; ++j) {
            if (offs[i] == offs[j]) {
                return 1;
            }
        }
    }
    return 0;
}

/**
 * A message is passed by const pointer (as a cog_provider_request_t
 * would reference it) and all four roles serialize to distinct tags.
 */
static int verify_read_only_usage(void)
{
    static const char user_text[] = "What is the weather?";
    const cog_message_t msgs[] = {
        {COG_MESSAGE_ROLE_SYSTEM, "Be concise.", 11U, NULL},
        {COG_MESSAGE_ROLE_USER, user_text, sizeof(user_text) - 1U, NULL},
        {COG_MESSAGE_ROLE_ASSISTANT, NULL, 0U, NULL},
        {COG_MESSAGE_ROLE_TOOL, "{}", 2U, "lookup"},
    };
    const size_t n = sizeof(msgs) / sizeof(msgs[0]);
    size_t i;

    for (i = 0U; i < n; ++i) {
        const cog_message_t *m = &msgs[i];
        if (m->content_len > 0U && m->content == NULL) {
            return 1;
        }
        if (role_index(m->role) != (int)i) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    unsigned failures = 0U;

#define CHECK(label, expr)                                                   \
    do {                                                                     \
        if ((expr) != 0) {                                                   \
            (void)fprintf(stderr, "FAIL: %s\n", label);                      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

    CHECK("role values", verify_role_values());
    CHECK("role switch", verify_role_switch());
    CHECK("zero-initialized message", verify_zero_initialized_message());
    CHECK("length-prefixed content", verify_length_prefixed_content());
    CHECK("tool role name", verify_tool_role_uses_name());
    CHECK("member layout", verify_member_layout());
    CHECK("read-only usage", verify_read_only_usage());

#undef CHECK

    if (failures == 0U) {
        (void)printf("test_message: ok\n");
    }
    return failures == 0U ? 0 : 1;
}
