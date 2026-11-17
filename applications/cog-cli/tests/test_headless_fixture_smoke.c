/** @file test_headless_fixture_smoke.c @brief Shared REAL fixture equivalence. */
#define _POSIX_C_SOURCE 200809L
#include "headless_fixture.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"fixture line %d: %s\n",__LINE__,#x);if(primary==COG_OK)primary=COG_ERROR_PROTOCOL;goto finish;} } while(0)
int main(int argc,char **argv)
{
    headless_fixture_t fixture={0};cog_result_t primary=COG_OK;
    CHECK(argc==8);
    const char *paths[6]={argv[1],argv[2],argv[3],argv[4],argv[5],argv[6]};
    primary=headless_fixture_init(&fixture,paths,1U,2U);CHECK(primary==COG_OK);
    primary=headless_fixture_open(&fixture);CHECK(primary==COG_OK);
    primary=headless_fixture_ready(&fixture,10000U);CHECK(primary==COG_OK);
    cog_cli_host_runtime_t *host=fixture.host;
    cog_cli_app_t *app=host->executors[host->primary_slot].app;CHECK(app!=NULL);
    CHECK(cog_cli_app_queue(app,"delegate round 1",strlen("delegate round 1"))==COG_OK);
    uint64_t deadline=headless_fixture_clock_ms()+20000U;
    bool complete=false,native_seen=false,metadata_seen=false;
    while(headless_fixture_clock_ms()<deadline){
        bool done=false;primary=headless_fixture_step(host,&done);CHECK(primary==COG_OK && !done);
        for(size_t i=2U;i<host->bounds.executors;++i)
            if(host->executors[i].agent!=0U && host->executors[i].owner_peer!=0U)native_seen=true;
        if(native_seen && !metadata_seen){
            char path[4096];int count=snprintf(path,sizeof(path),"%s/registry.json",argv[4]);
            CHECK(count>0 && (size_t)count<sizeof(path));
            CHECK(headless_fixture_registry(host,path)==COG_OK);metadata_seen=true;
        }
        cog_session_node_t *leaf=cog_session_active(app->session);
        if(leaf!=NULL){const cog_message_t *message=cog_node_data(leaf)->message;
            if(!app->active && message->role==COG_MESSAGE_ROLE_ASSISTANT && message->status==COG_MESSAGE_COMPLETE &&
                message->content!=NULL && strcmp(message->content,"PARENT_DONE_1")==0){complete=true;break;}}
    }
    CHECK(complete && native_seen && metadata_seen);
    deadline=headless_fixture_clock_ms()+5000U;bool free_groups=false;
    while(headless_fixture_clock_ms()<deadline){
        free_groups=true;
        for(size_t i=0U;i<host->group_capacity;++i)if(host->groups[i].phase!=CLI_GROUP_FREE)free_groups=false;
        if(free_groups)break;
        bool done=false;primary=headless_fixture_step(host,&done);CHECK(primary==COG_OK && !done);
    }
    CHECK(free_groups);
finish:;
    bool cleaned=false;
    primary=headless_fixture_close(&fixture,primary,&cleaned);
    if(primary!=COG_OK || !cleaned){fprintf(stderr,"fixture close rc=%d complete=%d\n",(int)primary,(int)cleaned);return 1;}
    puts("shared actual REAL fixture: PASS");return 0;
}
