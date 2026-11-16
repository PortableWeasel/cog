/** @file session_scenario.h @brief Test-owned actual UI client driving helpers. */
#ifndef COG_TEST_SESSION_SCENARIO_H
#define COG_TEST_SESSION_SCENARIO_H
#include "headless_fixture.h"
#include "view_internal.h"
#include "../../../libraries/cog-ipc/src/ipc_internal.h"
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>

typedef struct session_client {
    cog_cli_host_client_t *client;
    FILE *output;
    int input[2];
} session_client_t;
static inline cog_result_t session_client_open(session_client_t *c,const char *socket,
    const char *output,bool watch);
static inline cog_result_t session_tick(headless_fixture_t *f,session_client_t *a,
    session_client_t *b);
static inline bool session_online(const headless_fixture_t *f,const session_client_t *c);
static inline cog_result_t session_write(session_client_t *c,const char *text);
static inline cog_result_t session_disconnect(session_client_t *c);
static inline bool session_contains(const session_client_t *c,const char *text);
static inline cog_result_t session_client_close(session_client_t *c,cog_result_t primary);

static inline cog_result_t session_client_open(session_client_t *c,const char *socket,
    const char *output,bool watch)
{
    memset(c,0,sizeof(*c));c->input[0]=-1;c->input[1]=-1;
    if(pipe(c->input)!=0)return COG_ERROR_IO;
    c->output=fopen(output,"w+");if(c->output==NULL)return COG_ERROR_IO;
    cog_cli_host_client_config_t cfg={.socket_path=socket,.watch=watch,
        .terminal=false,.output=c->output,.diagnostic=stderr,.input_fd=c->input[0],
        .signal_fd=-1,.projection_bytes=8U*1024U*1024U,.reconnect_ms=3000U};
    return cog_cli_host_client_new(&c->client,&cfg);
}
static inline cog_result_t session_tick(headless_fixture_t *f,session_client_t *a,
    session_client_t *b)
{
    session_client_t *all[2]={a,b};
    for(size_t i=0U;i<2U;++i)if(all[i]!=NULL && all[i]->client!=NULL){
        cog_cli_host_runtime_step_t info={0};
        cog_result_t rc=cog_cli_host_client_step(all[i]->client,&info);
        if(rc!=COG_OK && rc!=COG_ERROR_AGAIN)return rc;
        if(info.done)return COG_ERROR_INVALID_STATE;
    }
    bool done=false;cog_result_t rc=headless_fixture_step(f->host,&done);
    return rc!=COG_OK?rc:done?COG_ERROR_INVALID_STATE:COG_OK;
}
static inline bool session_online(const headless_fixture_t *f,const session_client_t *c)
{
    return c->client!=NULL && c->client->phase==CLI_CLIENT_ONLINE &&
        c->client->have_view && c->client->hello.phase==CLI_CLIENT_REQUEST_ACKED &&
        c->client->rendered_sequence==f->host->projection->sequence;
}
static inline cog_result_t session_write(session_client_t *c,const char *text)
{
    size_t length=strlen(text);
    return write(c->input[1],text,length)==(ssize_t)length?COG_OK:COG_ERROR_IO;
}
static inline cog_result_t session_disconnect(session_client_t *c)
{
    /* Cut the actual connected socket. No client protocol/state is fabricated;
       its next service call discovers physical EOF and reconnects itself. */
    if(c->client==NULL || c->client->endpoint==NULL || c->client->endpoint->fd<0)
        return COG_ERROR_INVALID_STATE;
    return shutdown(c->client->endpoint->fd,SHUT_RDWR)==0?COG_OK:COG_ERROR_IO;
}
static inline bool session_contains(const session_client_t *c,const char *text)
{
    if(c->client==NULL || !c->client->have_view)return false;
    const cog_cli_display_snapshot_t *v=&c->client->banks[c->client->current].view.display;
    for(size_t i=0U;i<v->item_count;++i)
        if(v->items[i].text!=NULL && strcmp(v->items[i].text,text)==0)return true;
    return false;
}
static inline cog_result_t session_client_close(session_client_t *c,cog_result_t primary)
{
    cog_result_t rc=cog_cli_host_client_close(&c->client,primary);
    if(primary==COG_OK)primary=rc;
    for(size_t i=0U;i<2U;++i)if(c->input[i]>=0){
        if(close(c->input[i])!=0 && primary==COG_OK)primary=COG_ERROR_IO;
        c->input[i]=-1;
    }
    if(c->output!=NULL){if(fclose(c->output)!=0 && primary==COG_OK)primary=COG_ERROR_IO;c->output=NULL;}
    return primary;
}
#endif
