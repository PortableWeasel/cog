/** Direct real callback clock/state/parser oracles; no transport/provider stubs. */
#include "../src/delegation.c"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    cli_delegation_handle_t a={0},b={0};
    a.started=true;a.done=true;a.output_cached=true;a.deadline_ms=70U;
    b.started=true;b.done=true;b.output_cached=true;b.deadline_ms=90U;
    bool done=false;uint64_t deadline=12U;
    assert(tool_poll(&a,40U,&done,&deadline)==COG_OK && done && deadline==UINT64_MAX);
    assert(a.last_now_ms==40U && a.deadline_ms==70U);
    done=false;deadline=123U;
    assert(tool_poll(&a,39U,&done,&deadline)==COG_ERROR_INVALID_ARG);
    assert(!done && deadline==123U && a.last_now_ms==40U && a.deadline_ms==70U);
    assert(tool_poll(&a,40U,&done,&deadline)==COG_OK && done);
    assert(tool_cancel(&a,40U)==COG_OK && a.last_now_ms==40U);
    assert(tool_cancel(&a,39U)==COG_ERROR_INVALID_ARG && !a.cancelled);
    assert(tool_poll(&b,3U,&done,&deadline)==COG_OK && b.last_now_ms==3U && a.last_now_ms==40U);
    assert(tool_poll(&b,200U,NULL,&deadline)==COG_ERROR_NULL_PTR && b.last_now_ms==3U);
    assert(tool_poll(&b,200U,&done,NULL)==COG_ERROR_NULL_PTR && b.last_now_ms==3U);
    assert(tool_cancel(&b,4U)==COG_OK && b.last_now_ms==4U && a.last_now_ms==40U);
    cli_delegation_handle_t start={0};start.prepared=true;start.last_now_ms=8U;start.deadline_ms=10U;
    assert(tool_start(&start,7U)==COG_ERROR_INVALID_ARG && !start.started && start.last_now_ms==8U);
    assert(tool_start(&start,10U)==COG_ERROR_INVALID_STATE && !start.started && start.last_now_ms==10U);
    char arena[256];cli_delegation_handle_t parsed={0};
    parsed.operation=CLI_DELEGATE;parsed.arguments.data=(unsigned char *)arena;parsed.arguments.capacity=sizeof(arena);
    const char good[]="{\"tasks\":[{\"task\":\"one\"},{\"task\":\"two\\n\\u263a\"}]}";
    assert(arguments_parse(&parsed,good,sizeof(good)-1U)==COG_OK);
    assert(parsed.request.count==2U && strcmp(parsed.tasks[0],"one")==0 && parsed.task_lengths[1]==7U);
    parsed.request.count=0U;
    const char nul[]="{\"tasks\":[{\"task\":\"\\u0000\"}]}";
    assert(arguments_parse(&parsed,nul,sizeof(nul)-1U)==COG_ERROR_INVALID_ARG);
    const char duplicate[]="{\"tasks\":[\"one\"],\"tasks\":[\"two\"]}";
    assert(arguments_parse(&parsed,duplicate,sizeof(duplicate)-1U)==COG_ERROR_INVALID_ARG);
    const char empty[]="{\"tasks\":[]}";
    assert(arguments_parse(&parsed,empty,sizeof(empty)-1U)==COG_ERROR_INVALID_ARG);
    puts("delegation real callbacks clock/state/parser PASS");return 0;
}
