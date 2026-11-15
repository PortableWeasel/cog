#include "wire.h"
#include "yyjson.h"
#define JSMN_STATIC
#define JSMN_STRICT
#include "jsmn.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define ARENA_CAP (13u * WIRE_EVENT_CAP + 256u)
typedef union { max_align_t alignment; unsigned char bytes[ARENA_CAP]; } arena;

/* This only bounds nesting BEFORE a real strict JSON parser validates syntax.
 * Quoted braces and escaped quotes do not affect depth. No JSON is decoded here. */
static bool depth_ok(const char *p, size_t n) {
    size_t depth = 0;
    bool quoted = false, escape = false;
    for (size_t i = 0; i < n; ++i) {
        char c = p[i];
        if (quoted) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '[' || c == '{') {
            if (++depth > WIRE_DEPTH) return false;
        } else if (c == ']' || c == '}') {
            if (depth == 0) return false;
            --depth;
        }
    }
    return depth == 0 && !quoted;
}
static yyjson_doc *read_json(const char *p, size_t n, arena *a) {
    yyjson_alc alc;
    if (n > WIRE_EVENT_CAP || !depth_ok(p, n)) return NULL;
    if (!yyjson_alc_pool_init(&alc, a->bytes, sizeof(a->bytes))) return NULL;
    /* No permissive flags, no default allocator, no borrowed input mutation. */
    return yyjson_read_opts((char *)p, n, 0, &alc, NULL);
}
static bool append(char *p, size_t *len, size_t cap, const char *v, size_t n) {
    if (n >= cap - *len) return false;
    memcpy(p + *len, v, n);
    *len += n;
    p[*len] = '\0';
    return true;
}
static bool string_append(yyjson_val *v, char *p, size_t *n, size_t cap,
                          bool identifier) {
    if (!v || yyjson_is_null(v)) return true;
    if (!yyjson_is_str(v)) return false;
    const char *str = yyjson_get_str(v);
    size_t len = yyjson_get_len(v);
    if (identifier && memchr(str, 0, len)) return false;
    return append(p, n, cap, str, len);
}
static bool set_finish(wire_stream *s, yyjson_val *v) {
    if (!v || yyjson_is_null(v)) return true;
    if (!yyjson_is_str(v)) return false;
    if (s->finish[0]) return yyjson_equals_str(v, s->finish);
    const char *p = yyjson_get_str(v);
    if (!(yyjson_equals_str(v, "stop") || yyjson_equals_str(v, "tool_calls") ||
          yyjson_equals_str(v, "length") || yyjson_equals_str(v, "content_filter")))
        return false;
    memcpy(s->finish, p, yyjson_get_len(v) + 1);
    return true;
}
static wire_result decode_json(wire_stream *s, const char *p, size_t n) {
    arena a;
    yyjson_doc *doc = read_json(p, n, &a);
    if (!doc) return WIRE_JSON;
    yyjson_val *root = yyjson_doc_get_root(doc);
    wire_result result = WIRE_PROTOCOL;
    if (!yyjson_is_obj(root)) goto end;
    if (yyjson_obj_get(root, "error")) { result = WIRE_REMOTE; goto end; }
    yyjson_val *choices = yyjson_obj_get(root, "choices");
    if (!yyjson_is_arr(choices) || yyjson_arr_size(choices) > 1) goto end;
    yyjson_val *choice = yyjson_arr_get_first(choices);
    if (choice) {
        yyjson_val *idx = yyjson_obj_get(choice, "index");
        if (!yyjson_is_uint(idx) || yyjson_get_uint(idx) != 0) goto end;
        /* A finish frame may carry final deltas, later usage may have []. */
        yyjson_val *delta = yyjson_obj_get(choice, "delta");
        if (!yyjson_is_obj(delta)) goto end;
        if (s->finish[0]) {
            yyjson_val *content = yyjson_obj_get(delta, "content");
            yyjson_val *calls = yyjson_obj_get(delta, "tool_calls");
            if (content && !yyjson_is_null(content) && !yyjson_equals_str(content, "")) goto end;
            if (calls && !yyjson_is_null(calls) &&
                (!yyjson_is_arr(calls) || yyjson_arr_size(calls))) goto end;
        }
        if (!string_append(yyjson_obj_get(delta, "content"), s->text,
                           &s->text_len, sizeof(s->text), false)) goto end;
        yyjson_val *tools = yyjson_obj_get(delta, "tool_calls");
        if (tools && !yyjson_is_null(tools)) {
            if (!yyjson_is_arr(tools)) goto end;
            size_t i, count;
            yyjson_val *call;
            yyjson_arr_foreach(tools, i, count, call) {
                yyjson_val *index = yyjson_obj_get(call, "index");
                if (!yyjson_is_uint(index) || yyjson_get_uint(index) >= WIRE_TOOL_CAP)
                    goto end;
                wire_tool *t = &s->tools[(size_t)yyjson_get_uint(index)];
                yyjson_val *type = yyjson_obj_get(call, "type");
                if (type && !yyjson_equals_str(type, "function")) goto end;
                yyjson_val *fn = yyjson_obj_get(call, "function");
                if (fn && !yyjson_is_obj(fn)) goto end;
                if (!string_append(yyjson_obj_get(call, "id"), t->id,
                                   &t->id_len, sizeof(t->id), true) ||
                    !string_append(yyjson_obj_get(fn, "name"), t->name,
                                   &t->name_len, sizeof(t->name), true) ||
                    !string_append(yyjson_obj_get(fn, "arguments"), t->arguments,
                                   &t->arguments_len, sizeof(t->arguments), false)) goto end;
                t->seen = true;
            }
        }
        /* reasoning/reasoning_details are never merged into display text.
         * Production must preserve opaque continuation metadata separately. */
        if (!set_finish(s, yyjson_obj_get(choice, "finish_reason"))) goto end;
    }
    yyjson_val *usage = yyjson_obj_get(root, "usage");
    if (usage && !yyjson_is_null(usage)) {
        yyjson_val *in = yyjson_obj_get(usage, "prompt_tokens");
        yyjson_val *out = yyjson_obj_get(usage, "completion_tokens");
        if (!yyjson_is_uint(in) || !yyjson_is_uint(out)) goto end;
        s->prompt_tokens = yyjson_get_uint(in);
        s->completion_tokens = yyjson_get_uint(out);
        s->usage = true;
    }
    result = WIRE_OK;
end:
    yyjson_doc_free(doc);
    return result;
}
static wire_result event(wire_stream *s) {
    if (s->data_len == 0) return WIRE_OK;
    --s->data_len; /* SSE appends one LF per data field; remove final LF. */
    s->data[s->data_len] = '\0';
    if (s->done) return WIRE_PROTOCOL;
    if (s->data_len == 6 && memcmp(s->data, "[DONE]", 6) == 0) {
        if (!s->finish[0]) return WIRE_PROTOCOL;
        s->done = true;
    } else {
        /* Frame-level transaction: malformed later fields cannot partly commit. */
        wire_stream next = *s;
        wire_result r = decode_json(&next, s->data, s->data_len);
        if (r != WIRE_OK) return r;
        *s = next;
    }
    ++s->events;
    s->data_len = 0;
    return WIRE_OK;
}
static wire_result line(wire_stream *s) {
    if (s->line_len == 0) return event(s);
    if (s->line[0] == ':') { ++s->comments; return WIRE_OK; }
    size_t colon = 0;
    while (colon < s->line_len && s->line[colon] != ':') ++colon;
    if (colon == 4 && memcmp(s->line, "data", 4) == 0) {
        size_t start = colon < s->line_len ? colon + 1 : colon;
        if (start < s->line_len && s->line[start] == ' ') ++start;
        if (!append(s->data, &s->data_len, sizeof(s->data),
                    s->line + start, s->line_len - start) ||
            !append(s->data, &s->data_len, sizeof(s->data), "\n", 1))
            return WIRE_LIMIT;
    }
    return WIRE_OK;
}
void wire_init(wire_stream *s) { memset(s, 0, sizeof(*s)); }
wire_result wire_feed(wire_stream *s, const char *bytes, size_t n) {
    if (s->failed) return s->result;
    for (size_t i = 0; i < n; ++i) {
        char c = bytes[i];
        if (s->skip_lf && c == '\n') { s->skip_lf = false; continue; }
        s->skip_lf = false;
        wire_result r = WIRE_OK;
        if (c == '\r' || c == '\n') {
            r = line(s);
            s->line_len = 0;
            s->skip_lf = c == '\r';
        } else if (!append(s->line, &s->line_len, sizeof(s->line), &c, 1))
            r = WIRE_LIMIT;
        if (r != WIRE_OK) { s->failed = true; s->result = r; return r; }
    }
    return WIRE_OK;
}
wire_result wire_end(wire_stream *s) {
    if (s->failed) return s->result;
    wire_result r = WIRE_OK;
    if (!s->done || s->line_len || s->data_len) r = WIRE_TRUNCATED;
    else if (!strcmp(s->finish, "length") || !strcmp(s->finish, "content_filter"))
        r = WIRE_TRUNCATED;
    else {
        size_t tools = 0;
        for (size_t i = 0; i < WIRE_TOOL_CAP; ++i) {
            wire_tool *t = &s->tools[i];
            if (!t->seen) continue;
            ++tools;
            if (!t->id_len || !t->name_len) { r = WIRE_PROTOCOL; break; }
            for (size_t j = 0; j < i; ++j)
                if (s->tools[j].seen && !strcmp(t->id, s->tools[j].id)) r = WIRE_PROTOCOL;
            arena a;
            yyjson_doc *d = read_json(t->arguments, t->arguments_len, &a);
            if (!d || !yyjson_is_obj(yyjson_doc_get_root(d))) r = WIRE_JSON;
            yyjson_doc_free(d);
        }
        if ((tools > 0) != (strcmp(s->finish, "tool_calls") == 0)) r = WIRE_PROTOCOL;
    }
    if (r != WIRE_OK) { s->failed = true; s->result = r; }
    return r;
}

static const char good[] =
    ": OPENROUTER PROCESSING\r\n\r\n"
    "event: message\r\n"
    "data: {\r\ndata: \"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hi \\uD83D\\uDE00 \\\"\\\\\\n\"},\"finish_reason\":null}]}\r\n\r\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"echo\",\"arguments\":\"{\\\"value\\\":\"}}]},\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"\\\"café\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
    "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":9,\"completion_tokens\":7}}\n\n"
    "data: [DONE]\n\n";
static void replay(wire_stream *s, const char *p, size_t n, size_t chunk) {
    wire_init(s);
    for (size_t off = 0; off < n; off += chunk) {
        size_t take = n - off < chunk ? n - off : chunk;
        if (wire_feed(s, p + off, take) != WIRE_OK) break;
    }
}
static void test_stream(void) {
    wire_stream s;
    size_t n = strlen(good);
    for (size_t chunk = 1; chunk <= n; ++chunk) {
        replay(&s, good, n, chunk);
        assert(wire_end(&s) == WIRE_OK);
        assert(s.comments == 1 && s.events == 5 && s.usage);
        assert(s.prompt_tokens == 9 && s.completion_tokens == 7);
        assert(!strcmp(s.text, "Hi \xF0\x9F\x98\x80 \"\\\n"));
        assert(!strcmp(s.tools[0].arguments, "{\"value\":\"café\"}"));
    }
    /* Every possible single transport split, including UTF-8 and CRLF. */
    for (size_t split = 0; split <= n; ++split) {
        wire_init(&s);
        assert(wire_feed(&s, good, split) == WIRE_OK);
        assert(wire_feed(&s, good + split, n - split) == WIRE_OK);
        assert(wire_end(&s) == WIRE_OK);
    }
    for (size_t cut = 0; cut < n - 1; ++cut) {
        replay(&s, good, cut, 1);
        assert(wire_end(&s) != WIRE_OK);
    }
}
static wire_result check(const char *p) {
    wire_stream s;
    replay(&s, p, strlen(p), 1);
    return wire_end(&s);
}
static void test_failures(void) {
    assert(check("data: [DONE]\n\n") == WIRE_PROTOCOL);
    assert(check("data: {broken}\n\n") == WIRE_JSON);
    assert(check("data: {\"error\":{\"code\":429,\"message\":\"redacted\"}}\n\n") == WIRE_REMOTE);
    assert(check("data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]}\n\ndata: [DONE]\n\n") == WIRE_TRUNCATED);
    assert(check("data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"unknown\"}]}\n\n") == WIRE_PROTOCOL);
    assert(check("data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":4}]}}]}\n\n") == WIRE_PROTOCOL);
    assert(check("data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":0}}\n\ndata: [DONE]\n\n") == WIRE_OK);
    assert(check("data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"late\"},\"finish_reason\":\"stop\"}]}\n\n") == WIRE_PROTOCOL);
    assert(check("data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"length\"}]}\n\n") == WIRE_PROTOCOL);
    assert(check(": comment\r\rdata: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\r\rdata: [DONE]\r\r") == WIRE_OK);
    /* Remote error after accepted text leaves an explicitly incomplete turn. */
    wire_stream s;
    const char *partial = "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"partial\"}}]}\n\ndata: {\"error\":{\"code\":502}}\n\n";
    replay(&s, partial, strlen(partial), 1);
    assert(wire_end(&s) == WIRE_REMOTE && !strcmp(s.text, "partial") && !s.done);
    const char *atomic = "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"discard\",\"tool_calls\":42}}]}\n\n";
    replay(&s, atomic, strlen(atomic), 1);
    assert(wire_end(&s) == WIRE_PROTOCOL && s.text_len == 0);
    wire_init(&s);
    char huge[WIRE_EVENT_CAP + 1];
    memset(huge, 'x', sizeof(huge));
    assert(wire_feed(&s, huge, sizeof(huge)) == WIRE_LIMIT);
    /* Tool arguments are opaque strings until the turn has completed. */
    char invalid[sizeof(good)];
    memcpy(invalid, good, sizeof(good));
    char *where = strstr(invalid, "café");
    assert(where);
    *where = '\001'; /* JSON string now contains a raw control byte. */
    assert(check(invalid) == WIRE_JSON);
    /* Bad assembled arguments: valid wire JSON string, invalid tool JSON. */
    memcpy(invalid, good, sizeof(good));
    where = strstr(invalid, "value");
    assert(where);
    where[-3] = 'x'; /* Break the escaped quote in outer JSON: rejected immediately. */
    assert(check(invalid) != WIRE_OK);
    assert(check("data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"t\",\"function\":{\"name\":\"echo\",\"arguments\":\"{\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\ndata: [DONE]\n\n") == WIRE_JSON);
}
static void test_json(void) {
    arena a;
    yyjson_doc *d = read_json("\"\\uD83D\\uDE00\"", 14, &a);
    assert(d && yyjson_get_len(yyjson_doc_get_root(d)) == 4);
    yyjson_doc_free(d);
    const char *bad[] = {"\"\\uD800\"", "\"\\uDC00\"", "\"\xc0\xaf\"", "{\"x\":1} trailing", "[NaN]", "[1,]"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        d = read_json(bad[i], strlen(bad[i]), &a);
        assert(!d);
    }
    char deep[2 * (WIRE_DEPTH + 1) + 1];
    for (size_t i = 0; i < WIRE_DEPTH + 1; ++i) deep[i] = '[';
    for (size_t i = WIRE_DEPTH + 1; i < 2 * (WIRE_DEPTH + 1); ++i) deep[i] = ']';
    deep[sizeof(deep)-1] = 0;
    assert(!depth_ok(deep, strlen(deep)));
    d = read_json(deep + 1, strlen(deep) - 2, &a);
    assert(d); yyjson_doc_free(d);
    const char *braces = "{\"x\":\"[ { \\\" \\\\ ] }\"}";
    d = read_json(braces, strlen(braces), &a);
    assert(d); yyjson_doc_free(d);

    /* Bounded pool OOM, repeat/reinitialize cleanup, no fallback malloc. */
    union { max_align_t alignment; char bytes[96]; } tiny;
    yyjson_alc alc;
    yyjson_read_err err;
    for (int i = 0; i < 100; ++i) {
        assert(yyjson_alc_pool_init(&alc, tiny.bytes, sizeof(tiny.bytes)));
        d = yyjson_read_opts("{\"a\":[1,2,3,4,5,6,7,8]}", 23, 0, &alc, &err);
        assert(!d && err.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION);
    }
    assert(yyjson_alc_pool_init(&alc, a.bytes, 1024));
    for (int i = 0; i < 100; ++i) {
        d = yyjson_read_opts("{\"x\":1}", 7, 0, &alc, &err);
        assert(d);
        yyjson_doc_free(d);
    }
    assert(yyjson_alc_pool_init(&alc, a.bytes, sizeof(a.bytes)));
    yyjson_mut_doc *m = yyjson_mut_doc_new(&alc);
    assert(m);
    yyjson_mut_val *obj = yyjson_mut_obj(m);
    yyjson_mut_doc_set_root(m, obj);
    static const char value[] = "quote\" slash\\ newline\n tab\t emoji \xf0\x9f\x98\x80\0tail";
    assert(yyjson_mut_obj_add_strn(m, obj, "value", value, sizeof(value)-1));
    size_t n = 0;
    yyjson_write_err we;
    char *encoded = yyjson_mut_write_opts(m, 0, &alc, &n, &we);
    assert(encoded && strstr(encoded, "\\u0000tail"));
    arena b;
    d = read_json(encoded, n, &b);
    assert(d);
    yyjson_val *v = yyjson_obj_get(yyjson_doc_get_root(d), "value");
    assert(yyjson_get_len(v) == sizeof(value)-1 && !memcmp(yyjson_get_str(v), value, sizeof(value)-1));
    yyjson_doc_free(d);
    alc.free(alc.ctx, encoded);
    /* Writer allocation failure uses the supplied tiny allocator too. */
    yyjson_alc small;
    assert(yyjson_alc_pool_init(&small, tiny.bytes, sizeof(tiny.bytes)));
    encoded = yyjson_mut_write_opts(m, 0, &small, &n, &we);
    assert(!encoded && we.code == YYJSON_WRITE_ERROR_MEMORY_ALLOCATION);
    assert(yyjson_mut_obj_add_str(m, obj, "bad", "\xc0\xaf"));
    encoded = yyjson_mut_write_opts(m, 0, &alc, &n, &we);
    assert(!encoded && we.code == YYJSON_WRITE_ERROR_INVALID_STRING);
    yyjson_mut_doc_free(m);

    jsmn_parser parser;
    jsmntok_t tokens[16];
    jsmn_init(&parser);
    assert(jsmn_parse(&parser, "\"\\uD800\"", 8, tokens, 16) == 1);
    jsmn_init(&parser);
    assert(jsmn_parse(&parser, "\"\xc0\xaf\"", 4, tokens, 16) == 1);
    jsmn_init(&parser);
    assert(jsmn_parse(&parser, "[1,2,3]", 7, tokens, 1) == JSMN_ERROR_NOMEM);
    jsmntok_t many[80];
    jsmn_init(&parser);
    assert(jsmn_parse(&parser, deep, strlen(deep), many, 80) == 33);
}
static int fixture(const char *path, const char *kind) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fixture open"); return 1; }
    wire_stream s;
    wire_init(&s);
    int c;
    while ((c = fgetc(f)) != EOF) {
        char byte = (char)c;
        if (wire_feed(&s, &byte, 1) != WIRE_OK) break;
    }
    bool io_ok = !ferror(f);
    (void)fclose(f);
    wire_result r = wire_end(&s);
    if (!io_ok || r != WIRE_OK || !s.usage ||
        (!strcmp(kind, "tool") && !s.tools[0].seen) ||
        (!strcmp(kind, "text") && !s.text_len)) {
        fprintf(stderr, "fixture failed: result=%d usage=%d done=%d\n", (int)r, s.usage, s.done);
        return 1;
    }
    printf("fixture PASS: events=%zu comments=%zu text_bytes=%zu prompt_tokens=%llu completion_tokens=%llu\n",
           s.events, s.comments, s.text_len, (unsigned long long)s.prompt_tokens,
           (unsigned long long)s.completion_tokens);
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 3) return fixture(argv[1], argv[2]);
    if (argc != 1) return 2;
    test_json();
    test_stream();
    test_failures();
    puts("PASS: JSON Unicode/escaping/roundtrip/OOM/depth + fragmented SSE/tool/finish/error contracts");
    return 0;
}
