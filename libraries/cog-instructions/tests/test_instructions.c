#define _POSIX_C_SOURCE 200809L
#include <cog/instructions.h>
#include <cog/tool.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static cog_instruction_limits_t limits(void) {
    return (cog_instruction_limits_t){32, 16, 4096, 8192, 4096, 131072, 64, 16, 16, 16};
}
static const cog_tool_t read_tool = {
    .name = "read", .description = "Read files", .input_schema = "{\"type\":\"object\"}"};
static const cog_tool_t edit_tool = {
    .name = "edit", .description = "Edit files", .input_schema = "{\"type\":\"object\"}"};
static const char skill_text[] =
    "---\nname: test\ndescription: 'Test ''quoted'' skill'\nallowed-tools: read\nmetadata:\n  "
    "author: \"Some\\u0020Person\"\n---\nSKILL BODY\n";
static const char role_text[] =
    "---\ncog-agent: 1\nname: reader\ndescription: >\n  First line\n  second line\nmodel: "
    "m\ntools: read\nskills: test\nmax-output-tokens: 12\n---\nROLE BODY\n";
static cog_instruction_source_t source(cog_instruction_kind_t kind, const char *path,
                                       const char *text) {
    return (cog_instruction_source_t){kind, "root", path, "", NULL, 0, text, strlen(text)};
}
static void catalogs(void) {
    cog_instruction_limits_t l = limits();
    cog_instruction_source_t src[] = {
        source(COG_INSTRUCTION_HOST, "host.md", "HOST\n"),
        source(COG_INSTRUCTION_SKILL, "skills/test/SKILL.md", skill_text),
        source(COG_INSTRUCTION_AGENT, "agents/reader.md", role_text)};
    cog_instruction_catalog_t *cat = NULL;
    cog_instruction_diagnostic_t e;
    CHECK(cog_instruction_catalog_new(&cat, &l, 3, src, 3, &e) == COG_OK);
    CHECK(e.result == COG_OK);
    const cog_instruction_entry_t *entries;
    size_t count;
    CHECK(cog_instruction_catalog_view(cat, &entries, &count) == COG_OK && count == 2);
    CHECK(strcmp(entries[0].description, "First line second line\n") == 0);
    CHECK(strcmp(entries[1].description, "Test 'quoted' skill") == 0);
    cog_prompt_tool_binding_t bind[] = {{&read_tool, COG_PROMPT_TOOL_ASK},
                                        {&edit_tool, COG_PROMPT_TOOL_ALLOW}};
    cog_instruction_selection_t selection = {.snapshot_id = 4,
                                             .workspace_root_id = "root",
                                             .scope = "sub",
                                             .agent_name = "reader",
                                             .model_id = "m",
                                             .template_id = "t",
                                             .template_revision = 1,
                                             .max_context_bytes = 8192,
                                             .max_output_tokens = 100,
                                             .max_tool_rounds = 3,
                                             .bindings = bind,
                                             .binding_count = 2};
    cog_instruction_source_t add[] = {
        source(COG_INSTRUCTION_AGENTS, "AGENTS.md", "ROOT RULE\n"),
        source(COG_INSTRUCTION_AGENTS, "sub/AGENTS.md", "SUB RULE\n"),
        source(COG_INSTRUCTION_RESOURCE, "skills/test/file.txt", "RESOURCE\n")};
    add[1].scope = "sub";
    add[2].owner_skill = "test";
    cog_instruction_snapshot_t *snap = NULL;
    CHECK(cog_instruction_snapshot_new(&snap, cat, &l, &selection, add, 3, &e) == COG_OK);
    cog_instruction_manifest_t m;
    CHECK(cog_instruction_snapshot_view(snap, &m) == COG_OK);
    CHECK(m.tool_count == 1 && m.tools[0].policy == COG_PROMPT_TOOL_ASK &&
          m.max_output_tokens == 12);
    CHECK(m.fragment_count == 8);
    CHECK(strcmp(m.fragments[3].text, "ROLE BODY\n") == 0);
    CHECK(strcmp(m.fragments[5].text, "SKILL BODY\n") == 0);
    CHECK(strcmp(m.fragments[7].text, "Tool read: decision required\n") == 0);
    cog_prompt_bundle_t bundle;
    CHECK(cog_instruction_prompt_view(snap, &bundle) == COG_OK &&
          bundle.tools[0].descriptor == &read_tool);
    cog_instruction_snapshot_t *restored = NULL;
    CHECK(cog_instruction_snapshot_restore(&restored, &l, &m, COG_INSTRUCTION_EXECUTE, bind, 2,
                                           &e) == COG_OK);
    CHECK(cog_instruction_snapshot_free(restored) == COG_OK);
    bind[0].policy = COG_PROMPT_TOOL_DENY;
    CHECK(cog_instruction_snapshot_restore(&restored, &l, &m, COG_INSTRUCTION_EXECUTE, bind, 2,
                                           &e) == COG_ERROR_DENIED);
    CHECK(restored == NULL);
    CHECK(cog_instruction_snapshot_restore(&restored, &l, &m, COG_INSTRUCTION_INSPECT, NULL, 0,
                                           &e) == COG_OK);
    CHECK(cog_instruction_prompt_view(restored, &bundle) == COG_ERROR_INVALID_STATE);
    CHECK(cog_instruction_snapshot_free(restored) == COG_OK);
    cog_prompt_fragment_t fragments[16];
    memcpy(fragments, m.fragments, m.fragment_count * sizeof(*fragments));
    fragments[0].text = "EVIL\n";
    fragments[0].text_bytes = 5;
    m.fragments = fragments;
    CHECK(cog_instruction_snapshot_restore(&restored, &l, &m, COG_INSTRUCTION_INSPECT, NULL, 0,
                                           &e) == COG_ERROR_PROTOCOL);
    CHECK(restored == NULL);
    CHECK(cog_instruction_catalog_free(cat) == COG_OK);
    CHECK(cog_instruction_prompt_view(snap, &bundle) == COG_OK &&
          strcmp(bundle.fragments[0].text, "HOST\n") == 0);
    CHECK(cog_instruction_snapshot_free(snap) == COG_OK);
}
static void parser_failures(void) {
    cog_instruction_limits_t l = limits();
    const char *cases[] = {"---\nname: test\nname: test\ndescription: yes\n---\n",
                           "---\nname: test\ndescription: yes\nunknown: x\n---\n",
                           "---\nname: test\ndescription: [bad]\n---\n",
                           "---\nname: test\ndescription: \"\\uD800\"\n---\n",
                           "---\nname: wrong\ndescription: yes\n---\n"};
    const cog_result_t codes[] = {COG_ERROR_PROTOCOL, COG_ERROR_UNSUPPORTED, COG_ERROR_UNSUPPORTED,
                                  COG_ERROR_PROTOCOL, COG_ERROR_PROTOCOL};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        cog_instruction_source_t s = source(COG_INSTRUCTION_SKILL, "test/SKILL.md", cases[i]);
        cog_instruction_catalog_t *c = NULL;
        CHECK(cog_instruction_catalog_new(&c, &l, 1, &s, 1, NULL) == codes[i]);
        CHECK(c == NULL);
    }
    cog_instruction_source_t s[] = {
        source(COG_INSTRUCTION_SKILL, "test/SKILL.md", skill_text),
        source(COG_INSTRUCTION_SKILL, "other/test/SKILL.md", skill_text)};
    cog_instruction_catalog_t *c = NULL;
    CHECK(cog_instruction_catalog_new(&c, &l, 1, s, 2, NULL) == COG_ERROR_PROTOCOL);
    s[1].precedence = 1;
    CHECK(cog_instruction_catalog_new(&c, &l, 1, s, 2, NULL) == COG_OK);
    const cog_instruction_entry_t *entries;
    size_t n;
    CHECK(cog_instruction_catalog_view(c, &entries, &n) == COG_OK && n == 1 &&
          strcmp(entries[0].path, s[1].path) == 0);
    cog_instruction_catalog_free(c);
}
static void write_file(const char *dir, const char *name, const char *data, size_t n) {
    char path[1024];
    CHECK(snprintf(path, sizeof(path), "%s/%s", dir, name) > 0);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    CHECK(write(fd, data, n) == (ssize_t)n);
    CHECK(close(fd) == 0);
}
static cog_result_t complete(cog_instruction_source_reader_t *r, size_t *steps) {
    bool done = false;
    cog_result_t rc = COG_OK;
    *steps = 0;
    while (!done) {
        CHECK(++*steps < 100000);
        rc = cog_instruction_source_poll(r, &done);
        CHECK(rc == COG_OK || done);
    }
    return rc;
}
static void readers(void) {
    char dir[] = "./instruction-reader-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char absolute[1024];
    CHECK(getcwd(absolute, sizeof(absolute)) != NULL);
    size_t n = strlen(absolute);
    CHECK(n + strlen(dir) + 2U < sizeof(absolute));
    absolute[n++] = '/';
    strcpy(absolute + n, dir + 2);
    char path[1024];
    CHECK(snprintf(path, sizeof(path), "%s/sub", dir) > 0);
    CHECK(mkdir(path, 0700) == 0);
    write_file(dir, "AGENTS.md", "root", 4);
    write_file(dir, "sub/AGENTS.md", "child", 5);
    write_file(dir, "b.md", "beta", 4);
    write_file(dir, "a.md", "alpha", 5);
    write_file(dir, "bad.md", "\xc0\x80", 2);
    cog_instruction_source_root_t root = {"r", absolute};
    cog_instruction_source_config_t config = {
        .roots = &root, .root_count = 1, .limits = {1, 1024, 64, 1024, 16, 8, 1, 1}};
    cog_instruction_source_reader_t *r = NULL;
    CHECK(cog_instruction_source_reader_new(&r, &config, NULL) == COG_OK);
    cog_instruction_source_request_t q = {COG_INSTRUCTION_SOURCE_READ, "r", "a.md", false};
    CHECK(cog_instruction_source_begin(r, &q) == COG_OK);
    size_t steps;
    CHECK(complete(r, &steps) == COG_OK && steps >= 5);
    const cog_instruction_source_item_t *items;
    size_t count;
    CHECK(cog_instruction_source_view(r, &items, &count) == COG_OK && count == 1 &&
          strcmp(items[0].text, "alpha") == 0);
    const unsigned char expected[32] = {0x8e, 0xd3, 0xf6, 0xad, 0x68, 0x5b, 0x95, 0x9e,
                                        0xad, 0x70, 0x22, 0x51, 0x8e, 0x1a, 0xf7, 0x6c,
                                        0xd8, 0x16, 0xf8, 0xe8, 0xec, 0x7c, 0xcd, 0xda,
                                        0x1e, 0xd4, 0x01, 0x8e, 0x8f, 0x22, 0x23, 0xf8};
    CHECK(memcmp(items[0].sha256, expected, 32) == 0);
    q.path = "../escape";
    CHECK(cog_instruction_source_begin(r, &q) == COG_ERROR_INVALID_ARG);
    CHECK(strcmp(items[0].text, "alpha") == 0);
    q.path = "absent";
    q.optional = true;
    CHECK(cog_instruction_source_begin(r, &q) == COG_OK);
    CHECK(complete(r, &steps) == COG_OK);
    CHECK(cog_instruction_source_view(r, &items, &count) == COG_OK && count == 1 &&
          !items[0].present);
    q = (cog_instruction_source_request_t){COG_INSTRUCTION_SOURCE_SCOPE, "r", "sub", false};
    CHECK(cog_instruction_source_begin(r, &q) == COG_OK);
    CHECK(complete(r, &steps) == COG_OK);
    CHECK(cog_instruction_source_view(r, &items, &count) == COG_OK && count == 2 &&
          strcmp(items[0].text, "root") == 0 && strcmp(items[1].text, "child") == 0);
    q = (cog_instruction_source_request_t){COG_INSTRUCTION_SOURCE_AGENT_SCAN, "r", "", false};
    CHECK(cog_instruction_source_begin(r, &q) == COG_OK);
    CHECK(complete(r, &steps) == COG_OK);
    CHECK(cog_instruction_source_view(r, &items, &count) == COG_OK && count == 4 &&
          strcmp(items[0].path, "AGENTS.md") == 0 && strcmp(items[1].path, "a.md") == 0 &&
          !items[1].content_loaded);
    q = (cog_instruction_source_request_t){COG_INSTRUCTION_SOURCE_READ, "r", "bad.md", false};
    CHECK(cog_instruction_source_begin(r, &q) == COG_OK);
    CHECK(complete(r, &steps) == COG_ERROR_PROTOCOL);
    CHECK(cog_instruction_source_view(r, &items, &count) == COG_ERROR_PROTOCOL);
    cog_instruction_diagnostic_t err;
    CHECK(cog_instruction_source_error(r, &err) == COG_OK && strcmp(err.path, "bad.md") == 0);
    q.path = "a.md";
    CHECK(cog_instruction_source_begin(r, &q) == COG_OK);
    CHECK(cog_instruction_source_cancel(r) == COG_OK);
    CHECK(complete(r, &steps) == COG_ERROR_ABORTED);
    CHECK(cog_instruction_source_reader_free(r) == COG_OK);
    const char *files[] = {"AGENTS.md", "sub/AGENTS.md", "b.md", "a.md", "bad.md"};
    for (size_t i = 0; i < 5; ++i) {
        CHECK(snprintf(path, sizeof(path), "%s/%s", dir, files[i]) > 0);
        CHECK(unlink(path) == 0);
    }
    CHECK(snprintf(path, sizeof(path), "%s/sub", dir) > 0);
    CHECK(rmdir(path) == 0);
    CHECK(rmdir(dir) == 0);
}
int main(void) {
    catalogs();
    parser_failures();
    readers();
    return 0;
}
