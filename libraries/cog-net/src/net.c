#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <cog/net.h>
#include <curl/curl.h>
#include <dlfcn.h>
#include <errno.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* All mutable state is owned by the caller's context. */
typedef struct net_item {
    cog_net_response_t view;
    char *body;
    char *headers;
    bool exposed;
} net_item_t;

typedef struct net_request {
    struct cog_net_ctx *ctx;
    cog_request_id_t id;
    CURL *easy;
    CURLM *multi;
    struct curl_slist *request_headers;
    char *input;
    char *header_buf;
    size_t header_len;
    net_item_t *items;
    char *item_body;
    char *item_headers;
    size_t head, count;
    uint64_t next_sequence;
    uint64_t start_ms, last_byte_ms, pause_ms;
    int status;
    uint32_t retry_after;
    bool retry_known, started, connected, response_started, active, paused, terminal;
    cog_result_t callback_error, pending_result;
    cog_net_error_kind_t pending_kind;
    const char *pending_message;
    bool pending_terminal;
} net_request_t;

struct cog_net_ctx {
    net_request_t *requests;
    size_t max_connections, request_bytes, header_bytes, chunk_bytes, chunks;
    uint32_t connect_ms, stall_ms, quantum, budget;
    uint64_t now, next_id;
    bool clock_set;
    char *ca_bundle;
    size_t global_selected, fair_next, service_next;
};

#define NO_SELECTION SIZE_MAX

static bool mul_ok(size_t a, size_t b, size_t *out)
{
    if (a != 0U && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
}
static char *copy_string(const char *s)
{
    size_t n = strlen(s) + 1U;
    char *p = malloc(n);
    if (p != NULL) memcpy(p, s, n);
    return p;
}
static net_request_t *find_request(const cog_net_ctx_t *ctx, cog_request_id_t id)
{
    size_t i;
    for (i = 0U; i < ctx->max_connections; ++i)
        if (ctx->requests[i].id == id && id != 0U) return &ctx->requests[i];
    return NULL;
}
static void detach(net_request_t *r)
{
    if (r->active) {
        (void)curl_multi_remove_handle(r->multi, r->easy);
        r->active = false;
    }
}
static void clear_request(net_request_t *r)
{
    detach(r);
    if (r->easy != NULL) curl_easy_cleanup(r->easy);
    if (r->multi != NULL) curl_multi_cleanup(r->multi);
    curl_slist_free_all(r->request_headers);
    free(r->input);
    free(r->header_buf);
    free(r->items);
    free(r->item_body);
    free(r->item_headers);
    memset(r, 0, sizeof(*r));
}
static void push_item(net_request_t *r, const char *body, size_t length,
                      bool terminal, cog_result_t outcome,
                      cog_net_error_kind_t kind, const char *message)
{
    cog_net_ctx_t *ctx = r->ctx;
    size_t index = (r->head + r->count) % (ctx->chunks + 1U);
    net_item_t *item = &r->items[index];
    memset(&item->view, 0, sizeof(item->view));
    item->exposed = false;
    if (length != 0U) memcpy(item->body, body, length);
    if (r->header_len != 0U) {
        memcpy(item->headers, r->header_buf, r->header_len + 1U);
        item->view.headers = item->headers;
    }
    item->view.chunk = length != 0U ? item->body : NULL;
    item->view.chunk_len = length;
    item->view.http_status = r->status;
    item->view.request_id = r->id;
    item->view.sequence = r->next_sequence++;
    item->view.is_complete = terminal;
    item->view.outcome = outcome;
    item->view.error_kind = kind;
    item->view.error_message = message;
    item->view.retry_after_ms = r->retry_after;
    item->view.retry_after_known = r->retry_known;
    ++r->count;
}
static void terminal(net_request_t *r, cog_result_t outcome,
                     cog_net_error_kind_t kind, const char *message)
{
    if (r->terminal) return;
    detach(r);
    r->terminal = true;
    push_item(r, NULL, 0U, true, outcome, kind, message);
}
/* libcurl invokes this after DNS, TCP and TLS have succeeded, including on
 * reused connections, immediately before beginning the HTTP transfer. */
static int connection_ready(void *data, char *primary_ip, char *local_ip,
                            int primary_port, int local_port)
{
    net_request_t *r = data;
    (void)primary_ip; (void)local_ip; (void)primary_port; (void)local_port;
    /* An unexpected library-level retry must never transmit again. */
    if (r->connected) return CURL_PREREQFUNC_ABORT;
    r->connected = true;
    if (r->ctx->budget != 0U) --r->ctx->budget;
    return CURL_PREREQFUNC_OK;
}
static size_t receive_body(char *ptr, size_t size, size_t nmemb, void *data)
{
    net_request_t *r = data;
    cog_net_ctx_t *ctx = r->ctx;
    size_t n, need, remaining, offset;
    if (!mul_ok(size, nmemb, &n)) { r->callback_error = COG_ERROR_LIMIT; return 0U; }
    if (n == 0U) return 0U;
    need = n / ctx->chunk_bytes + (n % ctx->chunk_bytes != 0U ? 1U : 0U);
    if (need > ctx->chunks) { r->callback_error = COG_ERROR_LIMIT; return 0U; }
    if (ctx->budget == 0U || need > ctx->chunks - r->count) {
        if (!r->paused) r->pause_ms = ctx->now;
        r->paused = true;
        return CURL_WRITEFUNC_PAUSE;
    }
    --ctx->budget;
    r->response_started = true;
    r->last_byte_ms = ctx->now;
    remaining = n;
    offset = 0U;
    while (remaining != 0U) {
        size_t part = remaining < ctx->chunk_bytes ? remaining : ctx->chunk_bytes;
        push_item(r, ptr + offset, part, false, COG_OK, COG_NET_ERROR_NONE, NULL);
        remaining -= part;
        offset += part;
    }
    return n;
}
static size_t receive_header(char *ptr, size_t size, size_t nmemb, void *data)
{
    net_request_t *r = data;
    cog_net_ctx_t *ctx = r->ctx;
    size_t n;
    if (!mul_ok(size, nmemb, &n)) { r->callback_error = COG_ERROR_LIMIT; return 0U; }
    if (ctx->budget == 0U) { if (!r->paused) r->pause_ms = ctx->now; r->paused = true; return CURL_WRITEFUNC_PAUSE; }
    --ctx->budget;
    r->response_started = true;
    r->last_byte_ms = ctx->now;
    if (n >= 5U && memcmp(ptr, "HTTP/", 5U) == 0) {
        const char *space = memchr(ptr, ' ', n);
        if (space != NULL && (size_t)(ptr + n - space) >= 4U &&
            isdigit((unsigned char)space[1]) && isdigit((unsigned char)space[2]) &&
            isdigit((unsigned char)space[3])) {
            r->status = (space[1]-'0')*100 + (space[2]-'0')*10 + (space[3]-'0');
            r->header_len = 0U;
            r->header_buf[0] = '\0';
            r->retry_known = false;
        }
    } else if (n <= 2U && r->status >= 200) {
        if (r->count >= ctx->chunks) {
            if (!r->paused) r->pause_ms = ctx->now;
            r->paused = true;
            return CURL_WRITEFUNC_PAUSE;
        }
        push_item(r, NULL, 0U, false, COG_OK, COG_NET_ERROR_NONE, NULL);
    } else if (n > 2U) {
        size_t len = n;
        size_t colon = 0U, k;
        while (len != 0U && (ptr[len-1U] == '\r' || ptr[len-1U] == '\n')) --len;
        while (colon < len && ptr[colon] != ':') {
            unsigned char c = (unsigned char)ptr[colon];
            if (!isalnum(c) && strchr("!#$%&'*+-.^_`|~", c) == NULL) return 0U;
            ++colon;
        }
        if (colon == 0U || colon == len) return 0U;
        for (k = colon + 1U; k < len; ++k) {
            unsigned char c = (unsigned char)ptr[k];
            if ((c < 32U && c != '\t') || c == 127U) return 0U;
        }
        if (len + 1U > ctx->header_bytes - r->header_len) {
            r->callback_error = COG_ERROR_LIMIT;
            return 0U;
        }
        memcpy(r->header_buf + r->header_len, ptr, len);
        r->header_len += len;
        r->header_buf[r->header_len++] = '\n';
        r->header_buf[r->header_len] = '\0';
        if (len >= 12U) {
            static const char name[] = "retry-after:";
            size_t i;
            bool match = true;
            for (i = 0U; i < sizeof(name)-1U; ++i)
                if (tolower((unsigned char)ptr[i]) != name[i]) match = false;
            if (match) {
                uint64_t seconds = 0U;
                i = sizeof(name)-1U;
                while (i < len && (ptr[i] == ' ' || ptr[i] == '\t')) ++i;
                if (i < len) {
                    size_t start = i;
                    while (i < len && isdigit((unsigned char)ptr[i])) {
                        seconds = seconds * 10U + (uint64_t)(ptr[i]-'0');
                        if (seconds > UINT32_MAX / 1000U) break;
                        ++i;
                    }
                    while (i < len && (ptr[i] == ' ' || ptr[i] == '\t')) ++i;
                    if (i == len && i > start && seconds <= UINT32_MAX / 1000U) {
                        r->retry_after = (uint32_t)(seconds * 1000U);
                        r->retry_known = true;
                    }
                }
            }
        }
    }
    return n;
}

/* Bind the audited CMake resolver profile to the actually loaded ELF object.
 * If either the loader identity or the digest cannot be established, fail
 * closed. The deployment must protect the library path against replacement. */
static cog_result_t verify_backend(void)
{
#ifdef COG_NET_CURL_SHA256
    curl_version_info_data *(*symbol)(CURLversion) = curl_version_info;
    void *address = NULL;
    Dl_info info;
    FILE *file;
    EVP_MD_CTX *hash;
    unsigned char buf[8192], digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0U;
    size_t n, i;
    char hex[65];
    cog_result_t result = COG_ERROR_UNSUPPORTED;
    if (sizeof(address) != sizeof(symbol)) return COG_ERROR_UNSUPPORTED;
    memcpy(&address, &symbol, sizeof(address));
    if (dladdr(address, &info) == 0 || info.dli_fname == NULL) return COG_ERROR_UNSUPPORTED;
    file = fopen(info.dli_fname, "rb");
    if (file == NULL) return errno == ENOMEM ? COG_ERROR_NOMEM : COG_ERROR_UNSUPPORTED;
    hash = EVP_MD_CTX_new();
    if (hash == NULL) { (void)fclose(file); return COG_ERROR_NOMEM; }
    if (hash != NULL && EVP_DigestInit_ex(hash, EVP_sha256(), NULL) == 1) {
        while ((n = fread(buf, 1U, sizeof(buf), file)) != 0U)
            if (EVP_DigestUpdate(hash, buf, n) != 1) goto done;
        if (ferror(file) || EVP_DigestFinal_ex(hash, digest, &digest_len) != 1 || digest_len != 32U) goto done;
        for (i = 0U; i < 32U; ++i) (void)snprintf(hex + 2U*i, 3U, "%02x", digest[i]);
        hex[64] = '\0';
        if (strcmp(hex, COG_NET_CURL_SHA256) == 0) result = COG_OK;
    }
done:
    EVP_MD_CTX_free(hash);
    (void)fclose(file);
    return result;
#else
    return COG_ERROR_UNSUPPORTED;
#endif
}

cog_result_t cog_net_new(cog_net_ctx_t **out, const cog_net_config_t *config)
{
    cog_net_ctx_t *ctx;
    const curl_version_info_data *version;
    CURL *probe;
    CURLcode probe_status;
    cog_result_t provenance_status;
    size_t total, i, j;
    if (out == NULL) return COG_ERROR_NULL_PTR;
    *out = NULL;
    if (config == NULL) return COG_ERROR_NULL_PTR;
    if (!config->host_runtime_initialized || config->dependency_memory != COG_NET_DEPENDENCY_MEMORY_DYNAMIC ||
        config->max_retries != 0U || config->retry_backoff_ms != 0U) return COG_ERROR_INVALID_ARG;
    if (config->dependency_memory_limit != 0U) return COG_ERROR_UNSUPPORTED;
    provenance_status = verify_backend();
    if (provenance_status != COG_OK) return provenance_status;
    version = curl_version_info(CURLVERSION_NOW);
    if (version == NULL || (version->features & CURL_VERSION_ASYNCHDNS) == 0 ||
        version->ares == NULL || version->ares[0] == '\0') return COG_ERROR_UNSUPPORTED;
    probe = curl_easy_init();
    if (probe == NULL) return COG_ERROR_NOMEM;
    probe_status = curl_easy_setopt(probe, CURLOPT_DNS_SERVERS, "127.0.0.1");
    if (probe_status != CURLE_OK) {
        curl_easy_cleanup(probe);
        return probe_status == CURLE_OUT_OF_MEMORY ? COG_ERROR_NOMEM : COG_ERROR_UNSUPPORTED;
    }
    curl_easy_cleanup(probe);
    ctx = calloc(1U, sizeof(*ctx));
    if (ctx == NULL) return COG_ERROR_NOMEM;
    ctx->max_connections = config->max_connections != 0U ? config->max_connections : 4U;
    ctx->request_bytes = config->max_request_bytes != 0U ? config->max_request_bytes : 1048576U;
    ctx->header_bytes = config->max_header_bytes != 0U ? config->max_header_bytes : 65536U;
    ctx->chunk_bytes = config->chunk_bytes != 0U ? config->chunk_bytes : 16384U;
    ctx->chunks = config->chunks_per_request != 0U ? config->chunks_per_request : 8U;
    ctx->connect_ms = config->connect_timeout_ms != 0U ? config->connect_timeout_ms : 10000U;
    ctx->stall_ms = config->stall_timeout_ms != 0U ? config->stall_timeout_ms : 30000U;
    ctx->quantum = config->service_quantum != 0U ? config->service_quantum : 16U;
    ctx->global_selected = NO_SELECTION;
    if (ctx->chunks == SIZE_MAX || ctx->header_bytes == SIZE_MAX ||
        !mul_ok(ctx->max_connections, sizeof(*ctx->requests), &total) ||
        !mul_ok(ctx->chunks + 1U, sizeof(net_item_t), &total) ||
        !mul_ok(ctx->chunks + 1U, ctx->chunk_bytes, &total) ||
        !mul_ok(ctx->chunks + 1U, ctx->header_bytes + 1U, &total) ||
        ctx->max_connections > (size_t)LONG_MAX || ctx->request_bytes == SIZE_MAX) {
        free(ctx); return COG_ERROR_INVALID_ARG;
    }
    ctx->requests = calloc(ctx->max_connections, sizeof(*ctx->requests));
    if (config->ca_bundle != NULL) ctx->ca_bundle = copy_string(config->ca_bundle);
    if (ctx->requests == NULL ||
        (config->ca_bundle != NULL && ctx->ca_bundle == NULL)) goto nomem;
    for (i = 0U; i < ctx->max_connections; ++i) {
        net_request_t *r = &ctx->requests[i];
        r->ctx = ctx;
        r->multi = curl_multi_init();
        r->input = malloc(ctx->request_bytes);
        r->header_buf = malloc(ctx->header_bytes + 1U);
        r->items = calloc(ctx->chunks + 1U, sizeof(*r->items));
        r->item_body = malloc((ctx->chunks + 1U) * ctx->chunk_bytes);
        r->item_headers = malloc((ctx->chunks + 1U) * (ctx->header_bytes + 1U));
        if (r->multi == NULL || r->input == NULL || r->header_buf == NULL || r->items == NULL ||
            r->item_body == NULL || r->item_headers == NULL) goto nomem;
        if (curl_multi_setopt(r->multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, 1L) != CURLM_OK ||
            curl_multi_setopt(r->multi, CURLMOPT_MAXCONNECTS, 1L) != CURLM_OK) goto nomem;
        for (j = 0U; j <= ctx->chunks; ++j) {
            r->items[j].body = r->item_body + j * ctx->chunk_bytes;
            r->items[j].headers = r->item_headers + j * (ctx->header_bytes + 1U);
        }
    }
    *out = ctx;
    return COG_OK;
nomem:
    cog_net_free(ctx);
    return COG_ERROR_NOMEM;
}

void cog_net_free(cog_net_ctx_t *ctx)
{
    size_t i;
    if (ctx == NULL) return;
    if (ctx->requests != NULL)
        for (i = 0U; i < ctx->max_connections; ++i) clear_request(&ctx->requests[i]);
    free(ctx->requests);
    free(ctx->ca_bundle);
    free(ctx);
}

static bool valid_method(const char *s)
{
    static const char extra[] = "!#$%&'*+-.^_`|~";
    if (*s == '\0') return false;
    for (; *s != '\0'; ++s)
        if (!isalnum((unsigned char)*s) && strchr(extra, *s) == NULL) return false;
    return true;
}
static bool valid_url(const char *s)
{
    const char *p;
    if (strncmp(s, "https://", 8U) != 0) return false;
    p = s + 8;
    if (*p == '\0' || *p == '/' || *p == '?' || *p == '#') return false;
    while (*p != '\0' && *p != '/' && *p != '?' && *p != '#') {
        if (*p == '@' || isspace((unsigned char)*p) || (unsigned char)*p < 32U) return false;
        ++p;
    }
    for (; *p != '\0'; ++p)
        if ((unsigned char)*p <= 32U || (unsigned char)*p == 127U) return false;
    return true;
}
static bool valid_headers(const char *s)
{
    const char *p = s;
    if (p == NULL || *p == '\0') return true;
    while (*p != '\0') {
        const char *start = p;
        while (*p != '\0' && *p != ':' && *p != '\n' && *p != '\r') {
            unsigned char c = (unsigned char)*p;
            if (!isalnum(c) && strchr("!#$%&'*+-.^_`|~", c) == NULL) return false;
            ++p;
        }
        if (p == start || *p != ':') return false;
        ++p;
        while (*p != '\0' && *p != '\n') {
            unsigned char c = (unsigned char)*p++;
            if ((c < 32U && c != '\t') || c == 127U) return false;
        }
        if (*p == '\n') ++p;
    }
    return true;
}
#define SETOPT(r, option, value) do { if (curl_easy_setopt((r)->easy, (option), (value)) != CURLE_OK) goto setup_failed; } while (0)
cog_result_t cog_net_request(cog_net_ctx_t *ctx, const char *method, const char *url,
                             const char *headers, const char *body, size_t body_len,
                             cog_request_id_t *out_id)
{
    net_request_t *r = NULL;
    size_t i, ml, ul, hl, used;
    char *m, *u, *h, *b, *line;
    if (ctx == NULL || method == NULL || url == NULL || out_id == NULL ||
        (body == NULL && body_len != 0U)) return COG_ERROR_NULL_PTR;
    if (!valid_method(method) || !valid_url(url) || !valid_headers(headers) ||
        (body != NULL && body_len == 0U) || body_len > (size_t)INT64_MAX) return COG_ERROR_INVALID_ARG;
    for (i = 0U; i < ctx->max_connections; ++i) if (ctx->requests[i].id == 0U) { r = &ctx->requests[i]; break; }
    if (r == NULL) return COG_ERROR_OVERFLOW;
    if (ctx->next_id == UINT64_MAX) return COG_ERROR_LIMIT;
    ml = strlen(method)+1U; ul = strlen(url)+1U; hl = headers != NULL ? strlen(headers)+1U : 1U;
    used = ctx->request_bytes;
    if (ml > used) return COG_ERROR_LIMIT;
    used -= ml;
    if (ul > used) return COG_ERROR_LIMIT;
    used -= ul;
    if (hl > used) return COG_ERROR_LIMIT;
    used -= hl;
    if (body_len > used) return COG_ERROR_LIMIT;
    m = r->input; u = m + ml; h = u + ul; b = h + hl;
    memcpy(m, method, ml); memcpy(u, url, ul);
    if (headers != NULL) memcpy(h, headers, hl); else *h = '\0';
    if (body_len != 0U) memcpy(b, body, body_len);
    r->header_len = 0U; r->header_buf[0] = '\0';
    r->next_sequence = 1U;
    r->easy = curl_easy_init();
    if (r->easy == NULL) return COG_ERROR_NOMEM;
    line = h;
    while (*line != '\0') {
        char *end = strchr(line, '\n');
        struct curl_slist *new_list;
        if (end != NULL) *end = '\0';
        new_list = curl_slist_append(r->request_headers, line);
        if (end != NULL) *end = '\n';
        if (new_list == NULL) goto setup_failed;
        r->request_headers = new_list;
        if (end == NULL) break;
        line = end + 1;
    }
    SETOPT(r, CURLOPT_URL, u);
    SETOPT(r, CURLOPT_CUSTOMREQUEST, m);
    if (strcmp(m, "HEAD") == 0) SETOPT(r, CURLOPT_NOBODY, 1L);
    SETOPT(r, CURLOPT_HTTPHEADER, r->request_headers);
    SETOPT(r, CURLOPT_PROXY, "");
    SETOPT(r, CURLOPT_FOLLOWLOCATION, 0L);
    SETOPT(r, CURLOPT_FRESH_CONNECT, 1L);
    SETOPT(r, CURLOPT_FORBID_REUSE, 1L);
    SETOPT(r, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    SETOPT(r, CURLOPT_SSL_ENABLE_ALPN, 0L);
    SETOPT(r, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    SETOPT(r, CURLOPT_NETRC, CURL_NETRC_IGNORED);
    SETOPT(r, CURLOPT_UNRESTRICTED_AUTH, 0L);
    SETOPT(r, CURLOPT_PREREQFUNCTION, connection_ready);
    SETOPT(r, CURLOPT_PREREQDATA, r);
    SETOPT(r, CURLOPT_SSL_VERIFYPEER, 1L);
    SETOPT(r, CURLOPT_SSL_VERIFYHOST, 2L);
    SETOPT(r, CURLOPT_NOSIGNAL, 1L);
    SETOPT(r, CURLOPT_PROTOCOLS_STR, "https");
    SETOPT(r, CURLOPT_CONNECTTIMEOUT_MS, (long)ctx->connect_ms);
    SETOPT(r, CURLOPT_WRITEFUNCTION, receive_body);
    SETOPT(r, CURLOPT_WRITEDATA, r);
    SETOPT(r, CURLOPT_HEADERFUNCTION, receive_header);
    SETOPT(r, CURLOPT_HEADERDATA, r);
    SETOPT(r, CURLOPT_PRIVATE, r);
    if (ctx->ca_bundle != NULL) SETOPT(r, CURLOPT_CAINFO, ctx->ca_bundle);
    if (body != NULL) {
        SETOPT(r, CURLOPT_POSTFIELDS, b);
        SETOPT(r, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
    }
    if (curl_multi_add_handle(r->multi, r->easy) != CURLM_OK) goto setup_failed;
    r->id = ++ctx->next_id; r->active = true; r->ctx = ctx;
    *out_id = r->id;
    return COG_OK;
setup_failed:
    if (r->easy != NULL) curl_easy_cleanup(r->easy);
    r->easy = NULL;
    curl_slist_free_all(r->request_headers); r->request_headers = NULL;
    return COG_ERROR_NOMEM;
}

cog_result_t cog_net_cancel(cog_net_ctx_t *ctx, cog_request_id_t id)
{
    net_request_t *r;
    if (ctx == NULL) return COG_ERROR_NULL_PTR;
    r = find_request(ctx, id);
    if (r == NULL) return COG_ERROR_NOT_FOUND;
    if (r->pending_terminal) {
        terminal(r, r->pending_result, r->pending_kind, r->pending_message);
        r->pending_terminal = false;
    }
    terminal(r, COG_ERROR_ABORTED, COG_NET_ERROR_CANCELLED, "cancelled");
    return COG_OK;
}
cog_result_t cog_net_request_release(cog_net_ctx_t *ctx, cog_request_id_t id)
{
    net_request_t *r;
    size_t index;
    char *input, *headers, *bodies, *item_headers;
    net_item_t *items;
    CURLM *multi;
    if (ctx == NULL) return COG_ERROR_NULL_PTR;
    r = find_request(ctx, id);
    if (r == NULL) return COG_ERROR_NOT_FOUND;
    index = (size_t)(r - ctx->requests);
    if (ctx->global_selected == index) ctx->global_selected = NO_SELECTION;
    input = r->input; headers = r->header_buf; items = r->items; multi = r->multi;
    bodies = r->item_body; item_headers = r->item_headers;
    detach(r);
    if (r->easy != NULL) curl_easy_cleanup(r->easy);
    curl_slist_free_all(r->request_headers);
    memset(r, 0, sizeof(*r));
    r->ctx = ctx; r->multi = multi; r->input = input; r->header_buf = headers; r->items = items;
    r->item_body = bodies; r->item_headers = item_headers;
    return COG_OK;
}
static uint64_t deadline(uint64_t start, uint32_t delta)
{
    return UINT64_MAX - start < delta ? UINT64_MAX : start + delta;
}
/* Capture dependency completion as bookkeeping even if no publication budget
 * remains. Detach immediately so a later timeout or cancel cannot replace it. */
static void capture_completion(net_request_t *r, CURLcode code)
{
    if (r->callback_error == COG_ERROR_LIMIT) {
        r->pending_result = COG_ERROR_LIMIT; r->pending_kind = COG_NET_ERROR_CAPACITY;
        r->pending_message = "response capacity exceeded";
    } else if (code == CURLE_OK) {
        r->pending_result = COG_OK; r->pending_kind = COG_NET_ERROR_NONE;
        r->pending_message = NULL;
    } else if (code == CURLE_OUT_OF_MEMORY) {
        r->pending_result = COG_ERROR_NOMEM; r->pending_kind = COG_NET_ERROR_DEPENDENCY;
        r->pending_message = "dependency allocation failed";
    } else if (code == CURLE_OPERATION_TIMEDOUT) {
        r->pending_result = COG_ERROR_TIMEOUT; r->pending_kind = COG_NET_ERROR_TIMEOUT;
        r->pending_message = "transport timed out";
    } else if (code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_RESOLVE_PROXY) {
        r->pending_result = COG_ERROR_IO; r->pending_kind = COG_NET_ERROR_DNS;
        r->pending_message = "DNS resolution failed";
    } else if (code == CURLE_SSL_CONNECT_ERROR || code == CURLE_PEER_FAILED_VERIFICATION ||
               code == CURLE_SSL_CERTPROBLEM || code == CURLE_SSL_CACERT_BADFILE) {
        r->pending_result = COG_ERROR_IO; r->pending_kind = COG_NET_ERROR_TLS;
        r->pending_message = "TLS failed";
    } else if (code == CURLE_COULDNT_CONNECT) {
        r->pending_result = COG_ERROR_IO; r->pending_kind = COG_NET_ERROR_CONNECT;
        r->pending_message = "connection failed";
    } else {
        r->pending_result = COG_ERROR_IO; r->pending_kind = COG_NET_ERROR_DEPENDENCY;
        r->pending_message = "transport failed";
    }
    r->pending_terminal = true;
    detach(r);
}
cog_result_t cog_net_service(cog_net_ctx_t *ctx, uint64_t now_ms, uint64_t *out_next_deadline_ms)
{
    size_t i;
    uint32_t drivers = 0U;
    uint64_t next = UINT64_MAX;
    if (ctx == NULL || out_next_deadline_ms == NULL) return COG_ERROR_NULL_PTR;
    if (ctx->clock_set && now_ms < ctx->now) return COG_ERROR_INVALID_ARG;
    ctx->clock_set = true; ctx->now = now_ms; ctx->budget = ctx->quantum;
    for (i = 0U; i < ctx->max_connections; ++i) {
        size_t index = (ctx->service_next + i) % ctx->max_connections;
        net_request_t *r = &ctx->requests[index];
        uint64_t d;
        int running, messages;
        CURLMcode mc;
        CURLMsg *msg;
        long curl_timeout = -1L;
        if (r->pending_terminal) {
            if (ctx->budget == 0U) { next = now_ms; continue; }
            terminal(r, r->pending_result, r->pending_kind, r->pending_message);
            r->pending_terminal = false;
            --ctx->budget;
            continue;
        }
        if (!r->active) continue;
        if (!r->started) { r->start_ms = now_ms; r->started = true; }
        d = r->response_started ? deadline(r->last_byte_ms, ctx->stall_ms) :
            (r->connected ? UINT64_MAX : deadline(r->start_ms, ctx->connect_ms));
        if (d != UINT64_MAX && now_ms >= d && !r->paused) {
            if (ctx->budget == 0U) { next = now_ms; continue; }
            terminal(r, COG_ERROR_TIMEOUT, COG_NET_ERROR_TIMEOUT, "transport timed out");
            --ctx->budget;
            continue;
        }
        if (r->paused && r->count >= ctx->chunks) continue;
        if (drivers >= ctx->quantum || ctx->budget == 0U) { next = now_ms; continue; }
        ++drivers;
        if (r->paused && r->count < ctx->chunks) {
            uint64_t elapsed = now_ms - r->pause_ms;
            if (r->response_started)
                r->last_byte_ms = UINT64_MAX - r->last_byte_ms < elapsed ? UINT64_MAX : r->last_byte_ms + elapsed;
            else
                r->start_ms = UINT64_MAX - r->start_ms < elapsed ? UINT64_MAX : r->start_ms + elapsed;
            r->paused = false;
            if (curl_easy_pause(r->easy, CURLPAUSE_CONT) != CURLE_OK) {
                r->pending_result = r->callback_error == COG_ERROR_LIMIT ? COG_ERROR_LIMIT : COG_ERROR_IO;
                r->pending_kind = r->callback_error == COG_ERROR_LIMIT ? COG_NET_ERROR_CAPACITY : COG_NET_ERROR_DEPENDENCY;
                r->pending_message = r->callback_error == COG_ERROR_LIMIT ? "response capacity exceeded" : "transport resume failed";
                r->pending_terminal = true;
                detach(r);
                next = now_ms;
                continue;
            }
        }
        if (ctx->budget == 0U) { next = now_ms; continue; }
        mc = curl_multi_perform(r->multi, &running);
        if (mc != CURLM_OK) return mc == CURLM_OUT_OF_MEMORY ? COG_ERROR_NOMEM : COG_ERROR_IO;
        /* One easy handle per multi: at most one DONE needs capture. */
        msg = curl_multi_info_read(r->multi, &messages);
        if (msg != NULL && msg->msg == CURLMSG_DONE && r->active) {
            capture_completion(r, msg->data.result);
            if (ctx->budget != 0U) {
                terminal(r, r->pending_result, r->pending_kind, r->pending_message);
                r->pending_terminal = false;
                --ctx->budget;
            } else next = now_ms;
        }
        if (!r->active) continue;
        if (curl_multi_timeout(r->multi, &curl_timeout) != CURLM_OK) return COG_ERROR_IO;
        if (!r->paused) {
            d = r->response_started ? deadline(r->last_byte_ms, ctx->stall_ms) :
                (r->connected ? UINT64_MAX : deadline(r->start_ms, ctx->connect_ms));
            if (d < next) next = d;
            if (curl_timeout >= 0L) {
                d = deadline(now_ms, curl_timeout > (long)UINT32_MAX ? UINT32_MAX : (uint32_t)curl_timeout);
                if (d < next) next = d;
            }
        }
    }
    ctx->service_next = (ctx->service_next + 1U) % ctx->max_connections;
    if (ctx->budget == 0U) next = now_ms;
    *out_next_deadline_ms = next;
    return COG_OK;
}
cog_result_t cog_net_peek_request(cog_net_ctx_t *ctx, cog_request_id_t id, cog_net_response_t *out_response)
{
    net_request_t *r;
    net_item_t *item;
    if (ctx == NULL || out_response == NULL) return COG_ERROR_NULL_PTR;
    r = find_request(ctx, id);
    if (r == NULL) return COG_ERROR_NOT_FOUND;
    if (r->count == 0U) return COG_ERROR_AGAIN;
    item = &r->items[r->head]; item->exposed = true; *out_response = item->view;
    return COG_OK;
}
cog_result_t cog_net_peek(cog_net_ctx_t *ctx, cog_net_response_t *out_response)
{
    size_t i;
    if (ctx == NULL || out_response == NULL) return COG_ERROR_NULL_PTR;
    if (ctx->global_selected != NO_SELECTION)
        return cog_net_peek_request(ctx, ctx->requests[ctx->global_selected].id, out_response);
    for (i = 0U; i < ctx->max_connections; ++i) {
        size_t index = (ctx->fair_next + i) % ctx->max_connections;
        if (ctx->requests[index].id != 0U && ctx->requests[index].count != 0U) {
            ctx->global_selected = index;
            return cog_net_peek_request(ctx, ctx->requests[index].id, out_response);
        }
    }
    return COG_ERROR_AGAIN;
}
cog_result_t cog_net_ack(cog_net_ctx_t *ctx, cog_request_id_t id, uint64_t sequence)
{
    net_request_t *r;
    size_t index;
    net_item_t *item;
    if (ctx == NULL) return COG_ERROR_NULL_PTR;
    r = find_request(ctx, id);
    if (r == NULL) return COG_ERROR_NOT_FOUND;
    if (r->count == 0U) return COG_ERROR_INVALID_STATE;
    item = &r->items[r->head];
    if (!item->exposed || item->view.sequence != sequence) return COG_ERROR_INVALID_STATE;
    item->exposed = false;
    r->head = (r->head + 1U) % (ctx->chunks + 1U); --r->count;
    index = (size_t)(r - ctx->requests);
    if (ctx->global_selected == index) {
        ctx->global_selected = NO_SELECTION;
        ctx->fair_next = (index + 1U) % ctx->max_connections;
    }
    return COG_OK;
}
cog_result_t cog_net_poll(cog_net_ctx_t *ctx, cog_net_response_t *out_response)
{
    return cog_net_peek(ctx, out_response);
}
int cog_net_fd(const cog_net_ctx_t *ctx) { (void)ctx; return -1; }
size_t cog_net_active_count(const cog_net_ctx_t *ctx)
{
    size_t i, count = 0U;
    if (ctx == NULL) return 0U;
    for (i = 0U; i < ctx->max_connections; ++i) if (ctx->requests[i].active) ++count;
    return count;
}
cog_result_t cog_net_active_ids(const cog_net_ctx_t *ctx, cog_request_id_t *out_ids, size_t max_ids, size_t *out_count)
{
    size_t i, n = 0U;
    if (ctx == NULL || out_count == NULL || (out_ids == NULL && max_ids != 0U)) return COG_ERROR_NULL_PTR;
    *out_count = cog_net_active_count(ctx);
    if (max_ids < *out_count) return COG_ERROR_OVERFLOW;
    for (i = 0U; i < ctx->max_connections; ++i) if (ctx->requests[i].active) out_ids[n++] = ctx->requests[i].id;
    return COG_OK;
}
cog_result_t cog_net_request_status(const cog_net_ctx_t *ctx, cog_request_id_t id, bool *out_active)
{
    net_request_t *r;
    if (ctx == NULL || out_active == NULL) return COG_ERROR_NULL_PTR;
    r = find_request(ctx, id);
    if (r == NULL) return COG_ERROR_NOT_FOUND;
    *out_active = r->active;
    return COG_OK;
}
