/* Private constructor-owned transport and observation storage. */
#ifndef COG_NET_INTERNAL_H
#define COG_NET_INTERNAL_H
#include <cog/net.h>
#include <curl/curl.h>
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
    uint64_t observation_sequence, bytes_in, bytes_out;
    uint64_t reported_in, reported_out;
    bool wire_done, observation_done, observation_stalled;
    cog_result_t wire_result;
    cog_net_error_kind_t wire_kind;
} net_request_t;

struct cog_net_ctx {
    net_request_t *requests;
    size_t max_connections, request_bytes, header_bytes, chunk_bytes, chunks;
    uint32_t connect_ms, stall_ms, quantum, budget;
    uint64_t now, next_id;
    bool clock_set;
    cog_net_observer_fn observer;
    void *observer_user;
    cog_result_t observer_result;
    bool observing;
    char *ca_bundle;
    size_t global_selected, fair_next, service_next;
};

#endif
