/** @file view_internal.h
 * @brief O8 constructor-owned canonical projection and attach-client storage.
 * Private implementation layout only; no public transport/authority API.
 * projection.c owns both banks/ring; host_client.c owns each client record.
 * Source views, IPC receive buffers and caller FILE/fd borrows never become
 * execution/config/store authority. No service-time allocation or globals.
 */
#ifndef COG_CLI_VIEW_INTERNAL_H
#define COG_CLI_VIEW_INTERNAL_H
#include "projection.h"
#include "host_client.h"
#include "control.h"
#include <sys/un.h>
#ifdef __cplusplus
extern "C" {
#endif
#define CLI_VIEW_REPLAY_TYPE 1U /**< CGR1 DATA contains exact CGV1 replacement. */
#define CLI_VIEW_AGENTS 16U
#define CLI_VIEW_POLICIES 1U
#define CLI_CLIENT_LINE_BYTES 65536U /**< Includes NUL; longer input fails LIMIT. */
#define CLI_CLIENT_READ_BYTES 4096U
#define CLI_CLIENT_SMALL_FRAME 512U /**< CGL HELLO/ACK/CGR CURSOR including root ID. */

/** Separate copied arrays/text and encoded bytes for EACH bank. No pointers into
 * caller/other bank/IPC scratch survive decode. Text capacity=projection_bytes;
 * item capacity=max_display_bytes/14 (CGD1's minimum encoded row size).
 * Fixed agent/policy rows are constructor metadata. */
typedef struct cli_view_bank {
    cog_cli_projection_view_t view;
    cog_cli_display_item_t *items;
    cog_cli_agent_view_t agents[CLI_VIEW_AGENTS];
    cog_cli_policy_view_t policies[CLI_VIEW_POLICIES];
    char *text;
    unsigned char *wire;
    size_t wire_length;
} cli_view_bank_t;
struct cog_cli_projection {
    cog_cli_projection_config_t fixed; /**< session_id rebinds owned session. */
    char session[128];
    cog_ipc_replay_t *replay;
    cli_view_bank_t banks[2];
    size_t current, item_capacity;
    uint64_t sequence; /**< Sole canonical tail; checked before next staging. */
    bool committed;
};

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

/* Source ownership/algorithm notes (declarations above, bodies only after gate):
 * projection commit validates/encodes/decodes inactive bank under exact next
 * sequence before replay_append ONCE. Failed append leaves current bank/ring/
 * sequence untouched. The private ring has no other appender, so existing
 * replay_append guarantees the exact next sequence; assert that invariant, then
 * pointer swap only. No fallible post-append operation/rollback API is invented.
 * Client full candidate decode+render+checked flush precedes bank/cursor commit.
 * Failed/partial output freezes input and reports IO, never render ACK. Cursor
 * and resync ACK sends retain exact bytes under backpressure. No FILE buffering
 * allocation is introduced here; installed main supplies existing checked host
 * adapters. Borrowed embedding streams retain their explicit caller preconditions.
 */
#ifdef __cplusplus
}
#endif
#endif
