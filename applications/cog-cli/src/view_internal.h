#include <cog_cli/cli.h>
#ifndef COG_CLI_CLIENT_VIEW_INTERNAL_H
#define COG_CLI_CLIENT_VIEW_INTERNAL_H
#include "../../../libraries/cog-runtime/src/view_internal.h"
#include "host_client.h"
/** Connection discovery is independent of any retained effect-bearing command.
 * EOF/signal never becomes SHUTDOWN or ALLOW. DONE owns no effects to drain. */
typedef enum cli_client_phase {
    CLI_CLIENT_CONNECTING = 0, CLI_CLIENT_DISCOVERING, CLI_CLIENT_ONLINE,
    CLI_CLIENT_DETACHING, CLI_CLIENT_DONE
} cli_client_phase_t;
typedef enum cli_client_request_phase {
    CLI_CLIENT_REQUEST_FREE = 0, CLI_CLIENT_REQUEST_PENDING,
    CLI_CLIENT_REQUEST_FINAL, CLI_CLIENT_REQUEST_ACKED
} cli_client_request_phase_t;
/** Separate stop/wait COMMAND and CANCEL slots, plus a separate HELLO exchange.
 * Original encoded request bytes/identity retained until exact final result and
 * ACK-of-ACK; send OK copies bytes, never means admission/result/render.
 * ACKED retains the bounded latest result/ACK until reuse; stale result cannot
 * free/revise another pending request. A higher HELLO wire is NOT command-receipt
 * consumption. Reconnect never edits/re-IDs retained mutation bytes. */
typedef struct cli_client_request {
    cli_client_request_phase_t phase;
    cog_cli_control_lane_t lane;
    cog_cli_command_kind_t opcode;
    unsigned char *frame;
    size_t frame_capacity, frame_length;
    unsigned char ack[CLI_CLIENT_SMALL_FRAME];
    size_t ack_length;
    uint64_t wire, result_wire, ack_wire, related_agent, assigned_lease, input_serial;
    cog_result_t result;
    bool sent, ack_sent;
} cli_client_request_t;
struct cog_cli_host_client {
    cog_cli_host_client_config_t fixed; /**< FILE/fds borrowed, path rebound. */
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    cog_ipc_endpoint_t *endpoint; /**< One explicit reserved endpoint, reused on reconnect. */
    cog_ipc_peer_info_t proof; /**< Captured at one-unit readiness barrier BEFORE further IO. */
    bool authenticated, discovered, have_view, awaiting_resync, in_step, eof;
    bool stopping, cursor_pending;
    cli_client_phase_t phase;
    cog_result_t primary, final_input_result;
    uint64_t next_input_serial, last_input_serial;
    uint64_t nonce, lease, runtime, replay_generation, rendered_sequence, gap_last;
    char session[128]; /**< Discovered once; reconnect refuses another root. */
    uint64_t now_ms, deadline_ms, next_wire[10]; /**< Deadline=min only until phase resolves. */
    uint64_t last_server_wire[10];
    cli_client_request_t hello, requests[2]; /**< COMMAND0,CANCEL1. */
    unsigned char hello_frame[CLI_CLIENT_SMALL_FRAME];
    unsigned char cursor[CLI_CLIENT_SMALL_FRAME];
    size_t cursor_length;
    unsigned char *receive, *envelope_storage, *replay_storage, *command_body;
    size_t frame_capacity, envelope_capacity, replay_capacity, command_capacity;
    cli_view_bank_t banks[2]; /**< Immutable installed view vs independent decode candidate. */
    size_t current, item_capacity;
    char *render;
    size_t render_capacity, render_length;
    char line[CLI_CLIENT_LINE_BYTES], input[CLI_CLIENT_READ_BYTES];
    size_t line_length, input_length, input_cursor;
    uint64_t input_generation, input_sequence, line_generation, line_sequence;
    /**< Capture observed anchor when input bytes are READ, preserve it through
       buffered line completion. A pretyped POLICY/CONTINUE never acquires a
       later prompt/failed-leaf anchor merely because parsing was delayed. */
};


#endif
