/** @file test_app_stop.c @brief Cross-thread stop during timer backoff. */
#define _POSIX_C_SOURCE 200809L
#include "cog_cli/cli.h"
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)
static void *stopper(void *arg)
{
    struct timespec delay={0,20000000L};(void)nanosleep(&delay,NULL);
    cog_cli_app_stop(arg);return NULL;
}
int main(void)
{
    static const cog_mock_step_t steps[]={
        {COG_MOCK_STEP_ERROR,NULL,COG_ERROR_UNAVAILABLE,429,true,5000,"backoff"},
        {COG_MOCK_STEP_CHUNK,"must not send",COG_OK,0,false,0,NULL}};
    const cog_mock_script_t script={steps,2,0,COG_OK,0};
    cog_cli_options_t opts={.backend=COG_CLI_BACKEND_MOCK,.explicit_options=COG_CLI_OPT_BACKEND,.prompt="hello",.script=&script};
    cog_cli_app_t *app=NULL;REQUIRE(cog_cli_app_new(&app,&opts)==COG_OK);
    pthread_t thread;REQUIRE(pthread_create(&thread,NULL,stopper,app)==0);
    REQUIRE(cog_cli_app_run(app)==COG_OK);REQUIRE(pthread_join(thread,NULL)==0);
    cog_mock_state_t state;REQUIRE(cog_mock_provider_state(cog_cli_app_provider(app),&state)==COG_OK);
    REQUIRE(state.send_count==1U);
    cog_cli_display_snapshot_t display;REQUIRE(cog_cli_app_display_snapshot(app,&display)==COG_OK);
    REQUIRE(display.item_count==2U && display.items[1].status==COG_MESSAGE_CANCELLED);
    REQUIRE(cog_cli_app_submit(app,"continue",8)==COG_OK);
    REQUIRE(cog_cli_app_run(app)==COG_OK);
    REQUIRE(cog_mock_provider_state(cog_cli_app_provider(app),&state)==COG_OK && state.send_count==2U);
    cog_cli_app_free(app);return 0;
}
