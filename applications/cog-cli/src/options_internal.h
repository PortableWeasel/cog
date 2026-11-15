#ifndef COG_CLI_OPTIONS_INTERNAL_H
#define COG_CLI_OPTIONS_INTERNAL_H
#include <cog_cli/cli.h>
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
void cog_cli_options_defaults(cog_cli_options_t *options);
cog_result_t cog_cli_field_set(cog_cli_options_t *options, const cli_field_t *field,
                               const char *value);
#endif
