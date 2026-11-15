/** @file wire.h
 * @brief Private standalone research contract, deliberately outside Cog APIs.
 * Single thread; all buffers caller-owned; no network or implicit startup.
 * Feed arbitrary byte fragments. A failed stream retains incomplete text but
 * must never authorize a tool. Completion requires finish metadata + [DONE].
 * Feed errors are terminal, not retryable buffer-capacity errors. Production
 * delta retry/required-capacity semantics remain a future header-gate task.
 */
#ifndef PROVIDER_WIRE_H
#define PROVIDER_WIRE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define WIRE_EVENT_CAP 32768u
#define WIRE_TEXT_CAP 8192u
#define WIRE_TOOL_CAP 4u
#define WIRE_DEPTH 32u
typedef enum { WIRE_OK, WIRE_JSON, WIRE_LIMIT, WIRE_PROTOCOL,
               WIRE_REMOTE, WIRE_TRUNCATED } wire_result;
typedef struct {
    char id[128], name[128], arguments[4096];
    size_t id_len, name_len, arguments_len;
    bool seen;
} wire_tool;
typedef struct {
    char line[WIRE_EVENT_CAP], data[WIRE_EVENT_CAP], text[WIRE_TEXT_CAP];
    size_t line_len, data_len, text_len;
    wire_tool tools[WIRE_TOOL_CAP];
    char finish[32];
    uint64_t prompt_tokens, completion_tokens;
    size_t comments, events;
    bool skip_lf, done, failed, usage;
    wire_result result;
} wire_stream;
/** @brief Zero initialize caller storage. @param s Non-null output context. */
void wire_init(wire_stream *s);
/** @brief Consume n bytes, or mark failure. @param s Initialized context.
 * @param bytes Non-null input unless n=0. @param n Exact input length.
 * @return WIRE_OK or terminal error; after failure consumes nothing further.
 */
wire_result wire_feed(wire_stream *s, const char *bytes, size_t n);
/** @brief Validate EOF, terminal metadata, and completed tool JSON objects.
 * @param s Initialized context. @return WIRE_OK only for a complete turn.
 * length/content_filter finishes retain text but return WIRE_TRUNCATED.
 */
wire_result wire_end(wire_stream *s);
#endif
