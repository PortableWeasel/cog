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


#ifdef __cplusplus
}
#endif
#endif
