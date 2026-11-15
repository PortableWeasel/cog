/** @file toolkit_json.h @brief Private bounded strict codec and schema validator. */
#ifndef COG_TOOLKIT_JSON_H
#define COG_TOOLKIT_JSON_H
#include <cog/tool.h>
#include "../vendor/yyjson/prefix.h"
#include <yyjson.h>
typedef struct cog_toolkit_json {
    void *pool;
    size_t pool_bytes;
    size_t max_depth;
    yyjson_alc allocator;
    yyjson_doc *arguments;
    yyjson_doc *schema;
} cog_toolkit_json_t;
cog_result_t cog_toolkit_json_new(cog_toolkit_json_t *json,
    const char *arguments, size_t arguments_len, const char *schema,
    const cog_tool_async_config_t *config);
cog_result_t cog_toolkit_json_validate_output(const char *text, size_t length, const char *schema);
void cog_toolkit_json_free(cog_toolkit_json_t *json);
#endif
