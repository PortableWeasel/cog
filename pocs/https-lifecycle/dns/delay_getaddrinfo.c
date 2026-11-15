/** @file delay_getaddrinfo.c
 * Test-only LD_PRELOAD shim. Delays every getaddrinfo invocation by two
 * seconds, then returns EAI_AGAIN. It never calls DNS or modifies resolv.conf.
 * Loaded only into the isolated threaded-resolver probe process.
 */
#define _POSIX_C_SOURCE 200809L
#include <netdb.h>
#include <stdio.h>
#include <time.h>

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **out)
{
    (void)service;
    (void)hints;
    *out = NULL;
    fprintf(stderr, "controlled getaddrinfo entered node=%s\n", node);
    struct timespec remaining = {2, 0};
    while (nanosleep(&remaining, &remaining) != 0) { }
    fprintf(stderr, "controlled getaddrinfo returned\n");
    return EAI_AGAIN;
}
