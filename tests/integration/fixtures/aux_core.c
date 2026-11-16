/** @file aux_core.c
 * @brief Owned binary-CGW worker fixture. No environment-selected authority.
 * FXM1 application frames select test behavior only after authenticated READY:
 * opcode1 crash,2 noack,3 ackhang,4 load; byte5 is child load. Echo receipts
 * prove command receipt before a test requests native stop. They are not CGW.
 */
#define _POSIX_C_SOURCE 200809L
#include <cog/orchestrator.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ms(void)
{
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0) abort();
    return (uint64_t)ts.tv_sec*1000U+(uint64_t)ts.tv_nsec/1000000U;
}
static bool number(const char *s,uint64_t *out)
{
    uint64_t n=0;
    if(*s=='\0') return false;
    for(;*s!='\0';++s) {
        if(*s<'0'||*s>'9'||n>(UINT64_MAX-(uint64_t)(*s-'0'))/10U) return false;
        n=n*10U+(uint64_t)(*s-'0');
    }
    *out=n;return n!=0;
}
static cog_result_t send_control(cog_ipc_endpoint_t *ep,cog_worker_control_t *control)
{
    unsigned char bytes[37];size_t required=0;
    cog_result_t rc=cog_orchestrator_worker_encode(control,bytes,sizeof(bytes),&required);
    if(rc!=COG_OK) return rc;
    cog_ipc_message_t message={.data=bytes,.capacity=sizeof(bytes),.len=required};
    return cog_ipc_endpoint_send(ep,&message);
}
int main(int argc,char **argv)
{
    static const char *const flags[]={"--control-socket","--worker-incarnation","--worker-number","--parent-pid","--executor-slots","--frame-bytes"};
    uint64_t values[5];cog_ipc_endpoint_t *ep=NULL;
    if(argc!=14||strcmp(argv[1],"--worker")!=0) return 2;
    for(size_t i=0;i<6;++i) if(strcmp(argv[2+i*2],flags[i])!=0) return 2;
    for(size_t i=0;i<5;++i) if(!number(argv[5+i*2],&values[i])) return 2;
    if(values[2]!=(uint64_t)getppid()||values[3]<3||values[3]>18||values[4]>UINT32_MAX||values[4]<37) return 2;
    if(getenv("COG_AUX_MODE")!=NULL||getenv("COG_AUX_SESSION")!=NULL) return 2;
    cog_ipc_endpoint_config_t cfg={0};cfg.max_frame_bytes=(size_t)values[4];cfg.wipe_buffers=true;
    if(cog_ipc_endpoint_new_ex(&ep,&cfg)!=COG_OK) return 2;
    cog_result_t rc=cog_ipc_endpoint_connect(ep,argv[3],NULL);
    if(rc!=COG_OK&&rc!=COG_ERROR_AGAIN) { (void)cog_ipc_endpoint_free(ep);return 2; }
    cog_worker_control_t control={.kind=COG_WORKER_HELLO,.worker={values[0],values[1]}};
    bool hello=false,closing=false;unsigned mode=0;uint32_t active=0;
    uint64_t next_heartbeat=0,exit_at=UINT64_MAX,start=now_ms();
    unsigned char bytes[256];cog_ipc_message_t message={.data=bytes,.capacity=sizeof(bytes)};
    for(;;) {
        uint64_t now=now_ms(),deadline;
        rc=cog_ipc_endpoint_service(ep,now,&deadline);
        if(rc!=COG_OK&&rc!=COG_ERROR_AGAIN) break;
        if(!hello) {
            rc=send_control(ep,&control);
            if(rc==COG_OK) { hello=true;next_heartbeat=now; }
            else if(rc!=COG_ERROR_AGAIN&&rc!=COG_ERROR_INVALID_STATE) break;
        }
        if(hello&&!closing&&now>=next_heartbeat) {
            control.kind=COG_WORKER_HEARTBEAT;control.active_subagents=active;
            rc=send_control(ep,&control);
            if(rc==COG_OK) next_heartbeat=now+40;
            else if(rc!=COG_ERROR_AGAIN) break;
        }
        message.len=0;rc=cog_ipc_endpoint_recv(ep,&message);
        if(rc==COG_OK) {
            if(message.len==6&&memcmp(bytes,"FXM1",4)==0) {
                mode=bytes[4];if(mode==4) active=bytes[5];
                if(cog_ipc_endpoint_send(ep,&message)!=COG_OK) break;
                if(mode==1) exit_at=now+50;
                if(mode==2||mode==3) (void)signal(SIGTERM,SIG_IGN);
            } else {
                cog_worker_control_t request;
                if(cog_orchestrator_worker_decode(bytes,message.len,&request)!=COG_OK||
                   request.kind!=COG_WORKER_SHUTDOWN||request.worker.incarnation!=values[0]||request.worker.number!=values[1]) break;
                closing=true;
                if(mode!=2) {
                    request.kind=COG_WORKER_SHUTDOWN_ACK;request.outcome=COG_OK;
                    if(send_control(ep,&request)!=COG_OK) break;
                }
                if(mode!=2&&mode!=3) exit_at=now+50;
            }
        } else if(rc!=COG_ERROR_AGAIN) break;
        if(now>=exit_at) { (void)cog_ipc_endpoint_free(ep);return mode==1?1:0; }
        if(now-start>15000) break;
        (void)nanosleep(&(struct timespec){0,1000000L},NULL);
    }
    (void)cog_ipc_endpoint_free(ep);return 2;
}
