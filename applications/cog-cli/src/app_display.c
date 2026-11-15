/** @file app_display.c @brief Transactional portable visible snapshots. */
#include "app_internal.h"
#include <stdint.h>
#include <string.h>

cog_result_t cog_cli_app_display_snapshot(cog_cli_app_t *app, cog_cli_display_snapshot_t *out)
{
    if (app == NULL || out == NULL) return COG_ERROR_NULL_PTR;
    cog_session_node_t *node = cog_session_active(app->session);
    size_t count = 0U, bytes = 0U;
    for (cog_session_node_t *p = node; p != NULL; p = cog_node_parent(p)) {
        if (++count > CLI_NODES) return COG_ERROR_LIMIT;
        if (cog_node_data(p)->content_len > app->opts.max_context_bytes * 4U - bytes)
            return COG_ERROR_LIMIT;
        bytes += cog_node_data(p)->content_len;
    }
    size_t i = count;
    while (node != NULL) {
        const cog_node_data_t *data = cog_node_data(node);
        const cog_message_t *m = data->message;
        if (m == NULL) return COG_ERROR_INVALID_STATE;
        app->display_items[--i] = (cog_cli_display_item_t){data->node_id, m->role,
            m->status, m->content, m->content_len};
        node = cog_node_parent(node);
    }
    if (app->active && (app->provider_active || cog_core_state(app->core) == CLI_BACKOFF)) {
        app->display_items[count++] = (cog_cli_display_item_t){0U,
            COG_MESSAGE_ROLE_ASSISTANT, COG_MESSAGE_INCOMPLETE, app->text, app->text_len};
    }
    *out = (cog_cli_display_snapshot_t){cog_session_id(app->session), app->generation,
        app->display_sequence, app->display_items, count};
    return COG_OK;
}
static void put(unsigned char **p, uint64_t value, size_t width)
{
    for (size_t i = width; i != 0U; i--) *(*p)++ = (unsigned char)(value >> ((i - 1U) * 8U));
}
static uint64_t get(const unsigned char **p, size_t width)
{
    uint64_t value = 0U;
    for (size_t i = 0U; i < width; i++) value = (value << 8U) | *(*p)++;
    return value;
}
static bool visible_valid(const cog_cli_display_item_t *item)
{
    return item->role >= COG_MESSAGE_ROLE_SYSTEM && item->role <= COG_MESSAGE_ROLE_TOOL &&
        item->status >= COG_MESSAGE_COMPLETE && item->status <= COG_MESSAGE_FAILED &&
        item->text_len <= UINT32_MAX && (item->text != NULL || item->text_len == 0U) &&
        cog_cli_text_valid(item->text, item->text_len);
}
cog_result_t cog_cli_display_snapshot_encode(const cog_cli_display_snapshot_t *snapshot,
    void *output, size_t capacity, size_t *required)
{
    if (snapshot == NULL || required == NULL || (output == NULL && capacity != 0U)) return COG_ERROR_NULL_PTR;
    if (snapshot->session_id == NULL || snapshot->generation == 0U ||
        (snapshot->items == NULL && snapshot->item_count != 0U)) return COG_ERROR_INVALID_ARG;
    size_t sid = strlen(snapshot->session_id);
    if (sid == 0U || sid > 127U || !cog_cli_text_valid(snapshot->session_id, sid)) return COG_ERROR_INVALID_ARG;
    if (snapshot->item_count > UINT32_MAX) return COG_ERROR_LIMIT;
    size_t size = 4U + 1U + sid + 8U + 8U + 4U;
    for (size_t i = 0U; i < snapshot->item_count; i++) {
        const cog_cli_display_item_t *item = &snapshot->items[i];
        if (!visible_valid(item)) return COG_ERROR_INVALID_ARG;
        if (size > SIZE_MAX - 14U || item->text_len > SIZE_MAX - size - 14U) return COG_ERROR_LIMIT;
        size += 14U + item->text_len;
    }
    *required = size;
    if (capacity < size) return COG_ERROR_OVERFLOW;
    unsigned char *p = output;
    memcpy(p, "CGD1", 4U); p += 4U;
    put(&p, sid, 1U); memcpy(p, snapshot->session_id, sid); p += sid;
    put(&p, snapshot->generation, 8U); put(&p, snapshot->through_sequence, 8U);
    put(&p, snapshot->item_count, 4U);
    for (size_t i = 0U; i < snapshot->item_count; i++) {
        const cog_cli_display_item_t *item = &snapshot->items[i];
        put(&p, item->node_id, 8U); put(&p, (uint64_t)item->role, 1U);
        put(&p, (uint64_t)item->status, 1U); put(&p, item->text_len, 4U);
        if (item->text_len != 0U) memcpy(p, item->text, item->text_len);
        p += item->text_len;
    }
    return COG_OK;
}
cog_result_t cog_cli_display_snapshot_decode(const void *input, size_t input_len,
    cog_cli_display_snapshot_t *snapshot, cog_cli_display_item_t *items,
    size_t item_capacity, char *text, size_t text_capacity, size_t *required_items,
    size_t *required_text)
{
    if (input == NULL || snapshot == NULL || required_items == NULL || required_text == NULL ||
        (items == NULL && item_capacity != 0U) || (text == NULL && text_capacity != 0U)) return COG_ERROR_NULL_PTR;
    if (input_len < 4U) return COG_ERROR_PROTOCOL;
    const unsigned char *bytes = input;
    if (memcmp(bytes, "CGD1", 4U) != 0) return COG_ERROR_UNSUPPORTED;
    if (input_len < 5U) return COG_ERROR_PROTOCOL;
    const unsigned char *p = bytes + 4U;
    size_t sid = (size_t)get(&p, 1U);
    if (sid == 0U || sid > 127U || sid > input_len - 5U || input_len - 5U - sid < 20U ||
        !cog_cli_text_valid((const char *)p, sid)) return COG_ERROR_PROTOCOL;
    const unsigned char *session = p; p += sid;
    uint64_t generation = get(&p, 8U), sequence = get(&p, 8U);
    size_t count = (size_t)get(&p, 4U), text_bytes = sid + 1U;
    if (generation == 0U || count > (input_len - (size_t)(p - bytes)) / 14U) return COG_ERROR_PROTOCOL;
    const unsigned char *first = p;
    for (size_t i = 0U; i < count; i++) {
        size_t remain = input_len - (size_t)(p - bytes);
        if (remain < 14U) return COG_ERROR_PROTOCOL;
        uint64_t node = get(&p, 8U);
        cog_message_role_t role = (cog_message_role_t)get(&p, 1U);
        cog_message_status_t status = (cog_message_status_t)get(&p, 1U);
        size_t length = (size_t)get(&p, 4U);
        cog_cli_display_item_t item = {node, role, status, (const char *)p, length};
        if (length > remain - 14U || !visible_valid(&item) || length >= SIZE_MAX - text_bytes)
            return COG_ERROR_PROTOCOL;
        text_bytes += length + 1U; p += length;
    }
    if ((size_t)(p - bytes) != input_len) return COG_ERROR_PROTOCOL;
    *required_items = count; *required_text = text_bytes;
    if (item_capacity < count || text_capacity < text_bytes) return COG_ERROR_OVERFLOW;
    memcpy(text, session, sid); text[sid] = '\0';
    char *dest = text + sid + 1U;
    p = first;
    for (size_t i = 0U; i < count; i++) {
        uint64_t node = get(&p, 8U);
        cog_message_role_t role = (cog_message_role_t)get(&p, 1U);
        cog_message_status_t status = (cog_message_status_t)get(&p, 1U);
        size_t length = (size_t)get(&p, 4U);
        memcpy(dest, p, length); dest[length] = '\0';
        items[i] = (cog_cli_display_item_t){node, role, status, dest, length};
        dest += length + 1U; p += length;
    }
    *snapshot = (cog_cli_display_snapshot_t){text, generation, sequence, items, count};
    return COG_OK;
}
