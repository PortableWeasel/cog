/** @file openrouter_internal.h @brief Private bounded OpenRouter
 * implementation. One retained ordinary item plus a reserved terminal; decoded
 * final expansion advances only after next consumes each item. All buffers
 * reserved at connect.
 */
#ifndef COG_OPENROUTER_INTERNAL_H
#define COG_OPENROUTER_INTERNAL_H
#include "vendor/namespace.h"
#include "vendor/yyjson.h"
#include <cog/openrouter.h>
#include <cog/provider.h>
typedef struct or_tool
{
    char *id, *name, *arguments;
    size_t id_len, name_len, arguments_len;
    bool seen;
} or_tool;
typedef struct or_provider
{
    cog_provider_t base;
    const cog_endpoint_t *endpoint;
    cog_openrouter_config_t config;
    cog_request_id_t request_id;
    char *request, *line, *frame, *item_bytes, *opaque_bytes, *url, *headers;
    void *parser_pool, *writer_pool;
    void *preflight_parser_pool, *preflight_writer_pool;
    char *preflight_request;
    size_t line_len, frame_len, item_size, body_offset, text_len, opaque_len;
    or_tool *tools;
    yyjson_alc parser, writer;
    yyjson_mut_doc *opaque_doc;
    yyjson_mut_val *details;
    bool opaque_present, active, terminal, item_ready, done, skip_lf, stream;
    bool finish_seen, clock_seen, accepted, finalizing, used_receive, used_next;
    uint64_t now, sequence;
    size_t final_tool;
    unsigned final_stage;
    cog_finish_reason_t finish;
    cog_provider_delta_t item;
    cog_usage_t usage;
    bool usage_present, usage_final;
    cog_provider_error_t error;
    cog_result_t outcome;
} or_provider;
cog_result_t or_json_read(or_provider *p, const char *s, size_t n,
                          yyjson_doc **out);
cog_result_t or_encode(or_provider *p, const cog_provider_request_t *request,
                       size_t *len);
cog_result_t or_decode(or_provider *p, const char *s, size_t n);
cog_result_t or_eof(or_provider *p);
cog_result_t or_emit(or_provider *p, const cog_provider_delta_t *item);
cog_result_t or_expand(or_provider *p);
void or_fail(or_provider *p, cog_result_t code, cog_provider_error_kind_t kind,
             int status, uint32_t retry);
#endif
