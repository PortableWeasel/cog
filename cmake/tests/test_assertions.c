/** @file test_assertions.c @brief Assertions execute even in Release tests. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "Test workload would be removed: NDEBUG must be undefined"
#endif

int main(int argc, char **argv)
{
    unsigned effects = 0U;
    assert(++effects == 1U);
    if (effects != 1U) {
        return EXIT_FAILURE;
    }
    if (argc == 2 && strcmp(argv[1], "--fail") == 0) {
        puts("COG_TEST_ASSERTION_REACHED");
        if (fflush(stdout) != 0) {
            return EXIT_FAILURE;
        }
        assert(false && "COG_TEST_ASSERTION_FAILURE");
    } else if (argc != 1) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
