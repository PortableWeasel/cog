/** @file frontend.h @brief Private compatibility frontend callbacks. */
#ifndef COG_RUNTIME_FRONTEND_H
#define COG_RUNTIME_FRONTEND_H
#include "legacy_api.h"
typedef struct cli_frontend {
 cog_result_t (*open)(cog_cli_app_t *app);
 cog_result_t (*close)(cog_cli_app_t *app);
 cog_result_t (*log)(cog_cli_app_t *app,const char *text,size_t bytes);
 cog_result_t (*text)(cog_cli_app_t *app,const char *text,size_t bytes);
 cog_result_t (*usage)(cog_cli_app_t *app,const cog_session_node_t *node);
 cog_result_t (*ask)(cog_cli_app_t *app,const cog_message_tool_call_t *call);
 void (*error)(cog_cli_app_t *app,const char *label,cog_result_t error);
 cog_result_t (*idle)(void *app,uint32_t wait_ms);
 cog_result_t (*metadata)(cog_cli_app_t *app);
} cli_frontend_t;
cog_result_t cli_app_new_frontend(cog_cli_app_t **out,const cog_cli_options_t *options,const cli_frontend_t *frontend);
#endif
