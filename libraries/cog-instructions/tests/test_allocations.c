#define _POSIX_C_SOURCE 200809L
#include <cog/instructions.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%d: %s\n", __LINE__, #x);                                             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__wrap_malloc(size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_realloc(void *, size_t);
static size_t allocations;
static size_t fail_at;
void *__wrap_malloc(size_t n) {
    ++allocations;
    if (fail_at && allocations == fail_at)
        return NULL;
    return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t w) {
    ++allocations;
    if (fail_at && allocations == fail_at)
        return NULL;
    return __real_calloc(n, w);
}
void *__wrap_realloc(void *p, size_t n) {
    ++allocations;
    if (fail_at && allocations == fail_at)
        return NULL;
    return __real_realloc(p, n);
}
int main(void) {
    char dir[] = "./instruction-alloc-XXXXXX";
    CHECK(mkdtemp(dir));
    char path[1024], absolute[1024];
    CHECK(getcwd(absolute, sizeof(absolute)));
    size_t n = strlen(absolute);
    CHECK(n + strlen(dir) + 2U < sizeof(absolute));
    absolute[n++] = '/';
    strcpy(absolute + n, dir + 2);
    CHECK(snprintf(path, sizeof(path), "%s/a.md", dir) > 0);
    int fd = open(path, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0);
    CHECK(write(fd, "text", 4) == 4);
    CHECK(close(fd) == 0);
    cog_instruction_source_root_t root = {"r", absolute};
    cog_instruction_source_config_t config = {&root, 1, {1, 1024, 1024, 8192, 16, 8, 1, 1}};
    cog_instruction_source_reader_t *r = NULL;
    for (size_t k = 1; k < 30U; ++k) {
        allocations = 0;
        fail_at = k;
        cog_result_t rc = cog_instruction_source_reader_new(&r, &config, NULL);
        fail_at = 0;
        CHECK(rc == COG_OK || rc == COG_ERROR_NOMEM);
        if (rc == COG_OK)
            CHECK(cog_instruction_source_reader_free(r) == COG_OK);
        else
            CHECK(r == NULL);
    }
    CHECK(cog_instruction_source_reader_new(&r, &config, NULL) == COG_OK);
    allocations = 0;
    fail_at = 1;
    for (unsigned i = 0; i < 2U; ++i) {
        cog_instruction_source_request_t q = {i ? COG_INSTRUCTION_SOURCE_AGENT_SCAN
                                                : COG_INSTRUCTION_SOURCE_READ,
                                              "r", i ? "" : "a.md", false};
        CHECK(cog_instruction_source_begin(r, &q) == COG_OK);
        bool done = false;
        while (!done)
            CHECK(cog_instruction_source_poll(r, &done) == COG_OK);
        const cog_instruction_source_item_t *items;
        size_t count;
        CHECK(cog_instruction_source_view(r, &items, &count) == COG_OK && count == 1);
        CHECK(cog_instruction_source_cancel(r) == COG_OK);
    }
    CHECK(allocations == 0);
    fail_at = 0;
    CHECK(cog_instruction_source_reader_free(r) == COG_OK);
    cog_instruction_limits_t l = {16, 16, 4096, 8192, 4096, 131072, 64, 16, 16, 16};
    const char *text = "---\nname: test\ndescription: text\n---\nBODY";
    cog_instruction_source_t source = {
        COG_INSTRUCTION_SKILL, "r", "test/SKILL.md", "", NULL, 0, text, strlen(text)};
    for (size_t k = 1; k < 20U; ++k) {
        cog_instruction_catalog_t *c = NULL;
        allocations = 0;
        fail_at = k;
        cog_result_t rc = cog_instruction_catalog_new(&c, &l, 1, &source, 1, NULL);
        fail_at = 0;
        CHECK(rc == COG_OK || rc == COG_ERROR_NOMEM);
        if (c)
            cog_instruction_catalog_free(c);
    }
    CHECK(unlink(path) == 0);
    CHECK(rmdir(dir) == 0);
    return 0;
}
