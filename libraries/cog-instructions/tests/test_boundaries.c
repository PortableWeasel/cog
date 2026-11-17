#define _POSIX_C_SOURCE 200809L
#include <cog/instructions.h>
#include <cog/tool.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define T(x)                                                                                       \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static cog_instruction_limits_t lim(void) {
    return (cog_instruction_limits_t){16, 16, 4096, 8192, 4096, 131072, 64, 16, 16, 16};
}
static cog_instruction_source_t src(const char *text) {
    return (cog_instruction_source_t){
        COG_INSTRUCTION_SKILL, "r", "test/SKILL.md", "", NULL, 0, text, strlen(text)};
}
static void scalar_cases(void) {
    static const char *const scalars[] = {"plain text # comment",
                                          "\"line\\nnext\\t\\u263a\\ud83d\\ude00\"",
                                          "'one ''quote'''",
                                          "|\n  first\n\n  last",
                                          ">\n  first\n\n  last",
                                          "|\r\n  first\r\n  last",
                                          ">\n  first\n    indented\n  last"};
    static const char *const expected[] = {"plain text",
                                           "line\nnext\t\xe2\x98\xba\xf0\x9f\x98\x80",
                                           "one 'quote'",
                                           "first\n\nlast\n",
                                           "first\nlast\n",
                                           "first\nlast\n",
                                           "first\n  indented\nlast\n"};
    cog_instruction_limits_t l = lim();
    for (size_t i = 0; i < 7U; ++i) {
        char text[4096];
        T(snprintf(text, sizeof(text), "---\nname: test\ndescription: %s\n---\nbody", scalars[i]) >
          0);
        cog_instruction_source_t s = src(text);
        cog_instruction_catalog_t *c = NULL;
        T(cog_instruction_catalog_new(&c, &l, 1, &s, 1, NULL) == COG_OK);
        const cog_instruction_entry_t *e;
        size_t n;
        T(cog_instruction_catalog_view(c, &e, &n) == COG_OK && n == 1);
        T(strcmp(e->description, expected[i]) == 0);
        cog_instruction_catalog_free(c);
    }
}
static void limits_cases(void) {
    const char *text = "---\nname: test\ndescription: test\n---\nbody";
    cog_instruction_source_t s = src(text);
    cog_instruction_limits_t l = lim();
    cog_instruction_catalog_t *c = NULL;
    l.max_source_bytes = strlen(text) - 1U;
    T(cog_instruction_catalog_new(&c, &l, 1, &s, 1, NULL) == COG_ERROR_LIMIT);
    l = lim();
    l.max_metadata_bytes = 1;
    T(cog_instruction_catalog_new(&c, &l, 1, &s, 1, NULL) == COG_ERROR_LIMIT);
    l = lim();
    l.max_string_bytes = 4;
    T(cog_instruction_catalog_new(&c, &l, 1, &s, 1, NULL) == COG_ERROR_LIMIT);
    l = lim();
    l.max_total_bytes = 1;
    T(cog_instruction_catalog_new(&c, &l, 1, &s, 1, NULL) == COG_ERROR_LIMIT);
    l = lim();
    l.max_entries = 0;
    T(cog_instruction_catalog_new(&c, &l, 1, &s, 1, NULL) == COG_ERROR_INVALID_ARG);
    const char *bad[] = {
        "---\nname: test\ndescription: &anchor text\n---\n",
        "---\nname: test\ndescription: |+\n  text\n---\n",
        "---\nname: test\ndescription: yes\nmetadata:\n  key: one\n  key: two\n---\n",
        "---\nname: test\ndescription: a: b\n---\n",
        "---\nname: test\ndescription: \"\\u0000\"\n---\n",
        "---\nname:test\ndescription: text\n---\n",
        "---\nname: test\ndescription: 'text'#comment\n---\n",
        "---\nname: test\ndescription: -\n---\n"};
    cog_result_t codes[] = {COG_ERROR_UNSUPPORTED, COG_ERROR_UNSUPPORTED, COG_ERROR_PROTOCOL,
                            COG_ERROR_PROTOCOL,    COG_ERROR_PROTOCOL,    COG_ERROR_PROTOCOL,
                            COG_ERROR_PROTOCOL,    COG_ERROR_UNSUPPORTED};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        s = src(bad[i]);
        l = lim();
        T(cog_instruction_catalog_new(&c, &l, 1, &s, 1, NULL) == codes[i]);
        T(c == NULL);
    }
}
static void active_cases(void) {
    static const cog_tool_t t = {.name = "read", .description = NULL, .input_schema = NULL};
    cog_prompt_tool_binding_t b = {&t, COG_PROMPT_TOOL_ALLOW};
    cog_instruction_selection_t sel = {.snapshot_id = 1,
                                       .workspace_root_id = "r",
                                       .scope = "",
                                       .model_id = "m",
                                       .template_id = "t",
                                       .template_revision = 1,
                                       .max_context_bytes = 8192,
                                       .max_output_tokens = 4,
                                       .max_tool_rounds = 4,
                                       .bindings = &b,
                                       .binding_count = 1};
    cog_instruction_limits_t l = lim();
    cog_instruction_source_t s =
        src("---\nname: test\ndescription: test\nallowed-tools: missing\n---\nBODY");
    cog_instruction_catalog_t *c = NULL;
    T(cog_instruction_catalog_new(&c, &l, 1, &s, 1, NULL) == COG_OK);
    cog_instruction_snapshot_t *snap = NULL;
    T(cog_instruction_snapshot_new(&snap, c, &l, &sel, NULL, 0, NULL) == COG_OK);
    cog_instruction_manifest_t m;
    T(cog_instruction_snapshot_view(snap, &m) == COG_OK && m.available_skill_count == 0);
    cog_instruction_snapshot_free(snap);
    const char *skills[] = {"test", "test"};
    sel.skill_names = skills;
    sel.skill_count = 1;
    T(cog_instruction_snapshot_new(&snap, c, &l, &sel, NULL, 0, NULL) == COG_ERROR_DENIED);
    cog_instruction_catalog_free(c);
    s = src("---\nname: test\ndescription: test\n---\nBODY");
    T(cog_instruction_catalog_new(&c, &l, 1, &s, 1, NULL) == COG_OK);
    sel.skill_count = 2;
    T(cog_instruction_snapshot_new(&snap, c, &l, &sel, NULL, 0, NULL) == COG_ERROR_INVALID_ARG);
    sel.skill_count = 1;
    cog_instruction_source_t a = {COG_INSTRUCTION_RESOURCE, "r", "other/x", "", "test", 0, "x", 1};
    T(cog_instruction_snapshot_new(&snap, c, &l, &sel, &a, 1, NULL) == COG_ERROR_PROTOCOL);
    a.path = "test/x";
    T(cog_instruction_snapshot_new(&snap, c, &l, &sel, &a, 1, NULL) == COG_OK);
    T(cog_instruction_snapshot_view(snap, &m) == COG_OK);
    cog_instruction_snapshot_t *copy = NULL;
    b.policy = COG_PROMPT_TOOL_ASK;
    T(cog_instruction_snapshot_restore(&copy, &l, &m, COG_INSTRUCTION_EXECUTE, &b, 1, NULL) ==
      COG_ERROR_DENIED);
    cog_instruction_snapshot_free(snap);
    cog_instruction_catalog_free(c);
}
static void put(const char *p, const char *text, size_t n) {
    int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    T(fd >= 0);
    T(write(fd, text, n) == (ssize_t)n);
    T(close(fd) == 0);
}
static cog_result_t run(cog_instruction_source_reader_t *r) {
    bool done = false;
    cog_result_t rc;
    size_t n = 0;
    do {
        T(++n < 10000);
        rc = cog_instruction_source_poll(r, &done);
    } while (!done);
    return rc;
}
static void reader_cases(void) {
    char dir[] = "./instruction-boundary-XXXXXX";
    T(mkdtemp(dir));
    char absolute[1024];
    T(getcwd(absolute, sizeof(absolute)));
    size_t used = strlen(absolute);
    T(used + strlen(dir) + 2U < sizeof(absolute));
    absolute[used++] = '/';
    strcpy(absolute + used, dir + 2);
    char p[1024];
    T(snprintf(p, sizeof(p), "%s/test", dir) > 0);
    T(mkdir(p, 0700) == 0);
    T(snprintf(p, sizeof(p), "%s/test/SKILL.md", dir) > 0);
    put(p, "", 0);
    cog_instruction_source_root_t root = {"r", absolute};
    cog_instruction_source_config_t cfg = {&root, 1, {1, 1024, 8, 4096, 8, 4, 1, 1}};
    cog_instruction_source_reader_t *r = NULL;
    T(cog_instruction_source_reader_new(&r, &cfg, NULL) == COG_OK);
    cog_instruction_source_request_t q = {COG_INSTRUCTION_SOURCE_SKILL_SCAN, "r", "", false};
    T(cog_instruction_source_begin(r, &q) == COG_OK);
    T(run(r) == COG_OK);
    const cog_instruction_source_item_t *items;
    size_t n;
    T(cog_instruction_source_view(r, &items, &n) == COG_OK && n == 1 &&
      strcmp(items[0].path, "test/SKILL.md") == 0);
    /* Each scan owns an independent open-file description, even at root. */
    for (size_t repeat = 0; repeat < 2U; ++repeat) {
        T(cog_instruction_source_begin(r, &q) == COG_OK);
        T(run(r) == COG_OK);
        T(cog_instruction_source_view(r, &items, &n) == COG_OK && n == 1);
    }
    T(cog_instruction_source_begin(r, &q) == COG_OK);
    bool partial_done = false;
    for (size_t step = 0; step < 3U; ++step)
        T(cog_instruction_source_poll(r, &partial_done) == COG_OK && !partial_done);
    T(cog_instruction_source_cancel(r) == COG_OK);
    T(cog_instruction_source_begin(r, &q) == COG_OK);
    T(run(r) == COG_OK);
    T(cog_instruction_source_view(r, &items, &n) == COG_OK && n == 1);
    char ignored[1024], agent[1024];
    T(snprintf(ignored, sizeof(ignored), "%s/ignored-\xff.txt", dir) > 0);
    T(snprintf(agent, sizeof(agent), "%s/role.md", dir) > 0);
    put(ignored, "", 0);
    put(agent, "role", 4);
    q.operation = COG_INSTRUCTION_SOURCE_AGENT_SCAN;
    for (size_t repeat = 0; repeat < 2U; ++repeat) {
        T(cog_instruction_source_begin(r, &q) == COG_OK);
        T(run(r) == COG_OK);
        T(cog_instruction_source_view(r, &items, &n) == COG_OK && n == 1 &&
          strcmp(items[0].path, "role.md") == 0);
    }
    /* Skill scans likewise ignore unrelated regular files regardless of name bytes. */
    q.operation = COG_INSTRUCTION_SOURCE_SKILL_SCAN;
    T(cog_instruction_source_begin(r, &q) == COG_OK);
    T(run(r) == COG_OK);
    T(cog_instruction_source_view(r, &items, &n) == COG_OK && n == 1);
    T(unlink(ignored) == 0);
    T(unlink(agent) == 0);
    q.operation = COG_INSTRUCTION_SOURCE_READ;
    q.path = "test/SKILL.md";
    T(cog_instruction_source_begin(r, &q) == COG_OK);
    T(run(r) == COG_OK);
    T(cog_instruction_source_view(r, &items, &n) == COG_OK && items[0].content_loaded &&
      items[0].text_bytes == 0);
    T(cog_instruction_source_cancel(r) == COG_OK);
    T(cog_instruction_source_view(r, &items, &n) == COG_OK);
    put(p, "123456789", 9);
    T(cog_instruction_source_begin(r, &q) == COG_OK);
    T(run(r) == COG_ERROR_LIMIT);
    put(p, "12345678", 8);
    T(cog_instruction_source_begin(r, &q) == COG_OK);
    bool done = false;
    for (size_t i = 0; i < 4U; ++i)
        T(cog_instruction_source_poll(r, &done) == COG_OK && !done);
    put(p, "short", 5);
    T(run(r) == COG_ERROR_IO);
    T(unlink(p) == 0);
    T(symlink("/etc/passwd", p) == 0);
    T(cog_instruction_source_begin(r, &q) == COG_OK);
    T(run(r) == COG_ERROR_DENIED);
    T(unlink(p) == 0);
    T(mkfifo(p, 0600) == 0);
    T(cog_instruction_source_begin(r, &q) == COG_OK);
    T(run(r) == COG_ERROR_DENIED);
    T(unlink(p) == 0);
    put(p, "a\0b", 3);
    T(cog_instruction_source_begin(r, &q) == COG_OK);
    T(run(r) == COG_ERROR_PROTOCOL);
    T(unlink(p) == 0);
    T(snprintf(p, sizeof(p), "%s/test", dir) > 0);
    T(rmdir(p) == 0);
    T(cog_instruction_source_reader_free(r) == COG_OK);
    T(rmdir(dir) == 0);
}
int main(void) {
    scalar_cases();
    limits_cases();
    active_cases();
    reader_cases();
    return 0;
}
