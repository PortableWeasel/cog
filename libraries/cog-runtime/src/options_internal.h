#ifndef COG_CLI_OPTIONS_INTERNAL_H
#define COG_CLI_OPTIONS_INTERNAL_H
#include "legacy_api.h"
typedef enum cli_field_kind {
    CLI_STRING,
    CLI_BACKEND,
    CLI_POLICY,
    CLI_STORE,
    CLI_U32,
    CLI_SIZE
} cli_field_kind_t;
typedef struct cli_field {
    const char *flag, *short_flag, *key, *environment;
    uint32_t bit;
    size_t offset;
    cli_field_kind_t kind;
    bool zero;
} cli_field_t;
extern const cli_field_t cog_cli_fields[];
extern const size_t cog_cli_field_count;
/** Strict protocol UTF-8 scalars; paths do not use this validator. */
bool cog_cli_utf8_valid(const char *text);
/** Validate direct session-action inputs. Missing RECOVER assertion is DENIED;
 * malformed selectors/IDs are INVALID_ARG; source ID >127 bytes is LIMIT.
 * Store-backed adoption fencing is checked by the application after loading. */
cog_result_t cog_cli_session_options_validate(const cog_cli_options_t *options);
void cog_cli_options_defaults(cog_cli_options_t *options);
cog_result_t cog_cli_field_set(cog_cli_options_t *options, const cli_field_t *field,
                               const char *value);
#endif
