/** @file test_probe_ids_critic.c @brief Independent CGB1/G-L probe grammar join. */
#include "child_profile.h"
#include "options_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { (void)fprintf(stderr,"%d: %s\n",__LINE__,#x); exit(1); } } while (0)
void *__real_malloc(size_t n); void *__wrap_malloc(size_t n);
void *__real_calloc(size_t n,size_t s); void *__wrap_calloc(size_t n,size_t s);
void *__real_realloc(void *p,size_t n); void *__wrap_realloc(void *p,size_t n);
void __real_free(void *p); void __wrap_free(void *p);
char *__real_getenv(const char *n); char *__wrap_getenv(const char *n);
FILE *__real_fopen(const char *p,const char *m); FILE *__wrap_fopen(const char *p,const char *m);
static size_t allocations,reads;
void *__wrap_malloc(size_t n) { ++allocations; return __real_malloc(n); }
void *__wrap_calloc(size_t n,size_t s) { ++allocations; return __real_calloc(n,s); }
void *__wrap_realloc(void *p,size_t n) { ++allocations; return __real_realloc(p,n); }
void __wrap_free(void *p) { __real_free(p); }
char *__wrap_getenv(const char *n) { ++reads; return __real_getenv(n); }
FILE *__wrap_fopen(const char *p,const char *m) { ++reads; return __real_fopen(p,m); }
static cog_result_t credential(void *arg,const char *reference,char *out,size_t capacity,size_t *required)
{
    (void)arg; (void)reference; *required=sizeof("SYNTHETIC_CRITIC_KEY");
    if(capacity<*required)return COG_ERROR_OVERFLOW;
    memcpy(out,"SYNTHETIC_CRITIC_KEY",*required);return COG_OK;
}
static size_t word(const unsigned char *p)
{ return ((size_t)p[0]<<24U)|((size_t)p[1]<<16U)|((size_t)p[2]<<8U)|p[3]; }
static size_t rewrite_id(const unsigned char *frame,size_t length,unsigned char *out,const char *id,size_t n)
{
    size_t at=54U;
    for(size_t i=0;i<7U;++i) at+=4U+word(frame+at);
    size_t old=word(frame+at); CHECK(old<=length-at-4U);
    memcpy(out,frame,at);
    for(size_t i=0;i<4U;++i) out[at+i]=(unsigned char)(n>>((3U-i)*8U));
    memcpy(out+at+4U,id,n); memcpy(out+at+4U+n,frame+at+4U+old,length-at-4U-old);
    return length-old+n;
}
static void check_id(cog_cli_configuration_t *c,const cog_cli_bootstrap_probe_t *probe,
    const cog_net_config_t *transport,const unsigned char *valid,size_t length,const char *id,size_t n,bool raw_nul)
{
    char local[129]; CHECK(n<=128U);memcpy(local,id,n);local[n]='\0';
    cog_cli_options_t selector={.session_action=COG_CLI_SESSION_RESUME,.source_session_id=local};
    bool accepted=!raw_nul && cog_cli_session_options_validate(&selector)==COG_OK;
    unsigned char output[32768],modified[32768],before_output[32768];
    size_t required=999U;
    if(n<=127U && !raw_nul) {
        cog_cli_bootstrap_probe_t source=*probe;memcpy(source.session_id,local,n+1U);
        memset(output,0x55,sizeof(output));memcpy(before_output,output,sizeof(output));
        size_t old_alloc=allocations,old_reads=reads;
        cog_result_t rc=cog_cli_configuration_bootstrap_encode(c,&source,transport,output,sizeof(output),&required);
        CHECK(rc==(accepted?COG_OK:COG_ERROR_INVALID_ARG));CHECK(allocations==old_alloc && reads==old_reads);
        if(!accepted)CHECK(required==999U && memcmp(output,before_output,sizeof(output))==0);
    }
    size_t changed=rewrite_id(valid,length,modified,id,n),old_alloc=allocations,old_reads=reads;
    cog_cli_bootstrap_probe_t copied,before_probe;memset(&copied,0xa5,sizeof(copied));memcpy(&before_probe,&copied,sizeof(copied));
    cog_cli_configuration_t *decoded=(cog_cli_configuration_t *)(void *)&selector;
    cog_net_config_t got,before_transport;memset(&got,0xa5,sizeof(got));memcpy(&before_transport,&got,sizeof(got));
    char storage[16],before_storage[16];memset(storage,0x55,sizeof(storage));memcpy(before_storage,storage,sizeof(storage));
    required=999U;
    cog_result_t expected=n>127U?COG_ERROR_LIMIT:(accepted?COG_OK:COG_ERROR_PROTOCOL);
    CHECK(cog_cli_configuration_bootstrap_new(&decoded,modified,changed,(cog_net_ctx_t *)(void *)&selector,&copied)==expected);
    CHECK(reads==old_reads);
    if(accepted) {
        CHECK(decoded!=NULL && allocations==old_alloc+1U && strcmp(copied.session_id,local)==0);
        cog_cli_configuration_free(decoded);
    } else {
        CHECK(decoded==NULL && allocations==old_alloc && memcmp(&copied,&before_probe,sizeof(copied))==0);
    }
    old_alloc=allocations;
    CHECK(cog_cli_bootstrap_transport_decode(modified,changed,&got,storage,sizeof(storage),&required)==expected);
    CHECK(allocations==old_alloc && reads==old_reads);
    if(!accepted)CHECK(required==999U && memcmp(&got,&before_transport,sizeof(got))==0 && memcmp(storage,before_storage,sizeof(storage))==0);
}
int main(void)
{
    cog_cli_options_t o;cog_cli_options_defaults(&o);
    o.model_id="synthetic/model";o.credential_ref="host:critic";o.credential_read=credential;
    o.workspace_root="/work";o.store_dir="/state";
    o.explicit_options=COG_CLI_OPT_MODEL|COG_CLI_OPT_CREDENTIAL|COG_CLI_OPT_WORKSPACE|COG_CLI_OPT_STORE_DIR;
    cog_cli_configuration_t *c=NULL;CHECK(cog_cli_configuration_new(&c,&o,1)==COG_OK);
    CHECK(cog_cli_configuration_finish_host(c,(cog_net_ctx_t *)(void *)&o,"/work","/state")==COG_OK);
    cog_cli_bootstrap_probe_t probe={.session_id="root",.owner={1,2},.checkpoint={3}};
    cog_net_config_t transport={.host_runtime_initialized=true,.dependency_memory=COG_NET_DEPENDENCY_MEMORY_DYNAMIC};
    unsigned char wire[32768];size_t length;
    CHECK(cog_cli_configuration_bootstrap_encode(c,&probe,&transport,wire,sizeof(wire),&length)==COG_OK);
    for(unsigned byte=1U;byte<=127U;++byte) {
        char id[]={'a',(char)byte,'b'};check_id(c,&probe,&transport,wire,length,id,sizeof(id),false);
    }
    static const char *const more[]={"",".","..",".cog-domain.lock",".cog-domain.lockx","x.cog-domain.lock","a :@?*|<>\"","\xc2\x80","\xe0\xa0\x80","\xed\x9f\xbf","\xee\x80\x80","\xef\xbf\xbf","\xf4\x8f\xbf\xbf","\xc0\xaf","\xed\xa0\x80","\xf4\x90\x80\x80"};
    for(size_t i=0;i<sizeof(more)/sizeof(more[0]);++i) check_id(c,&probe,&transport,wire,length,more[i],strlen(more[i]),false);
    char maximum[128];memset(maximum,'s',sizeof(maximum));check_id(c,&probe,&transport,wire,length,maximum,127U,false);
    check_id(c,&probe,&transport,wire,length,maximum,128U,false);
    const char embedded[]={'a','\0','b'};check_id(c,&probe,&transport,wire,length,embedded,sizeof(embedded),true);
    cog_cli_configuration_free(c);return 0;
}
