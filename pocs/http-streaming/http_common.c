/**
 * @file http_common.c
 * @brief Shared helpers for the HTTP transports (see http_common.h).
 */

#include "http_common.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/** Static names for the http_err_t values. See http_err_name(). */
static const char *const k_error_names[] = {
    "none", "retryable", "client", "stall", "net", "parse", "timeout"
};

/** Number of entries in ::k_error_names. */
#define ERROR_NAME_COUNT (sizeof k_error_names / sizeof k_error_names[0])

const char *http_err_name(http_err_t e)
{
    size_t i = (size_t)e;
    if (i >= ERROR_NAME_COUNT) {
        return "unknown";
    }
    return k_error_names[i];
}

int http_split_url(const char *url, char *host, size_t host_cap,
                   uint16_t *port, const char **path)
{
    static const char k_prefix[] = "http://";
    static const size_t k_prefix_len = sizeof k_prefix - 1U;

    if (url == NULL || host == NULL || host_cap == 0U || port == NULL ||
        path == NULL) {
        return -1;
    }
    if (strncmp(url, k_prefix, k_prefix_len) != 0) {
        return -1;
    }

    const char *p = url + k_prefix_len;
    size_t host_len = strcspn(p, ":/");
    if (host_len == 0U || host_len >= host_cap) {
        return -1;
    }
    (void)memcpy(host, p, host_len);
    host[host_len] = '\0';

    const char *q = p + host_len;
    unsigned long parsed_port = 80UL;
    if (*q == ':') {
        char *end = NULL;
        parsed_port = strtoul(q + 1U, &end, 10);
        if (end == q + 1U || parsed_port == 0UL || parsed_port > 65535UL) {
            return -1;
        }
        q = end;
    }
    *port = (uint16_t)parsed_port;
    *path = (*q == '/') ? q : "/";
    return 0;
}
