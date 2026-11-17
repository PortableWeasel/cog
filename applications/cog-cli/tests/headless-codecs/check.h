/** @file check.h @brief Always-evaluated codec fixture assertions. */
#ifndef COG_CLI_CODEC_TEST_CHECK_H
#define COG_CLI_CODEC_TEST_CHECK_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(expr) do { if (!(expr)) { (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(EXIT_FAILURE); } } while (0)
#define OK(expr) CHECK((expr) == COG_OK)
static inline void test_fill(void *p, size_t n) { memset(p, 0xa5, n); }
static inline void test_unchanged(const void *p, size_t n)
{
    const unsigned char *s = p;
    for (size_t i = 0U; i < n; ++i) CHECK(s[i] == 0xa5U);
}
static inline void test_put(unsigned char *p, uint64_t v, size_t n)
{
    for (size_t i = 0U; i < n; ++i) p[i] = (unsigned char)(v >> ((n - i - 1U) * 8U));
}
static inline bool test_negative_control(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--negative-control") == 0) { CHECK(false); return true; }
    return false;
}
#endif
