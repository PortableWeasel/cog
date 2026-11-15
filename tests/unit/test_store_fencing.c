/** @file test_store_fencing.c
 * @brief Real exec/OS lock/crash tests and durable transaction syscall faults.
 * Link with --wrap=fsync,--wrap=renameat,--wrap=linkat,--wrap=write.
 * Fault wrappers call real syscalls except one deliberate EIO or abrupt exit.
 */
#define _POSIX_C_SOURCE 200809L
#include <cog/session_store.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { (void)fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c); exit(1); } } while (0)
/* Test-only fault state, never linked into the production library. */
static unsigned boundary, trigger;
static bool crash_fault, enabled, fail_temp_unlink;
static int pause_ready_fd=-1, pause_release_fd=-1;
static bool reject_allocations;
static size_t allocation_attempts, allocation_failure;
void *__real_malloc(size_t n);
void *__real_calloc(size_t n,size_t size);
void *__real_realloc(void *p,size_t n);
char *__real_strdup(const char *text);
void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t n,size_t size);
void *__wrap_realloc(void *p,size_t n);
char *__wrap_strdup(const char *text);
static bool allocation_fails(void)
{
    ++allocation_attempts;
    return reject_allocations || (allocation_failure != 0U && allocation_attempts == allocation_failure);
}
void *__wrap_malloc(size_t n) { return allocation_fails() ? NULL : __real_malloc(n); }
void *__wrap_calloc(size_t n,size_t size) { return allocation_fails() ? NULL : __real_calloc(n,size); }
void *__wrap_realloc(void *p,size_t n) { return allocation_fails() ? NULL : __real_realloc(p,n); }
char *__wrap_strdup(const char *text) { return allocation_fails() ? NULL : __real_strdup(text); }
int __real_unlinkat(int dir,const char *path,int flags);
int __wrap_unlinkat(int dir,const char *path,int flags);
int __real_fsync(int fd);
int __real_renameat(int olddir,const char *oldname,int newdir,const char *newname);
int __real_linkat(int olddir,const char *oldname,int newdir,const char *newname,int flags);
ssize_t __real_write(int fd,const void *p,size_t n);
int __wrap_fsync(int fd);
int __wrap_renameat(int olddir,const char *oldname,int newdir,const char *newname);
int __wrap_linkat(int olddir,const char *oldname,int newdir,const char *newname,int flags);
ssize_t __wrap_write(int fd,const void *p,size_t n);
static bool fail_now(void)
{
    if (!enabled) return false;
    ++boundary;
    if (boundary != trigger) return false;
    if (crash_fault) _exit(77);
    errno=EIO;return true;
}
int __wrap_fsync(int fd) { return fail_now() ? -1 : __real_fsync(fd); }
int __wrap_renameat(int a,const char *b,int c,const char *d)
{ return fail_now() ? -1 : __real_renameat(a,b,c,d); }
int __wrap_linkat(int a,const char *b,int c,const char *d,int flags)
{
    if(pause_ready_fd>=0){
        char ready='R';CHECK(__real_write(pause_ready_fd,&ready,1U)==1);CHECK(read(pause_release_fd,&ready,1U)==1);
        pause_ready_fd=-1;
    }
    return fail_now() ? -1 : __real_linkat(a,b,c,d,flags);
}
int __wrap_unlinkat(int dir,const char *path,int flags)
{
    if(fail_temp_unlink && strcmp(path,".generation.tmp")==0){fail_temp_unlink=false;errno=EIO;return -1;}
    return fail_now() ? -1 : __real_unlinkat(dir,path,flags);
}
ssize_t __wrap_write(int fd,const void *p,size_t n)
{ return fail_now() ? -1 : __real_write(fd,p,n); }
static void cleanup(const char *path)
{
    DIR *d=opendir(path);CHECK(d!=NULL);struct dirent *de;
    while((de=readdir(d))!=NULL){
        if(strcmp(de->d_name,".")==0 || strcmp(de->d_name,"..")==0)continue;
        char p[1024];CHECK(snprintf(p,sizeof(p),"%s/%s",path,de->d_name)>0);
        struct stat st;CHECK(lstat(p,&st)==0);if(S_ISDIR(st.st_mode))cleanup(p);else CHECK(unlink(p)==0);
    }
    CHECK(closedir(d)==0);CHECK(rmdir(path)==0);
}
static cog_store_t *store(const char *base,bool owned)
{ cog_store_t *s=NULL;CHECK(cog_store_new(&s,&(cog_store_config_t){.base_dir=base,.require_owner=owned,.max_record_bytes=1048576U})==COG_OK);return s; }
static cog_session_t *session(const char *id)
{ cog_session_t *s=NULL;CHECK(cog_session_new(&s,&(cog_session_config_t){.id=id,.max_nodes=16U,.max_payload_bytes=65536U})==COG_OK);return s; }
static cog_session_node_t *proposal(cog_session_t *s)
{
    const cog_message_tool_call_t c={.id="call",.name="effect",.arguments="{}",.arguments_len=2U};
    const cog_message_t m={.role=COG_MESSAGE_ROLE_ASSISTANT,.tool_calls=&c,.tool_call_count=1U};
    cog_session_node_t *n=NULL;CHECK(cog_session_append(s,&m,&n)==COG_OK);return n;
}
static int child_probe(const char *base,bool hold)
{
    cog_store_t *s=store(base,true);cog_store_owner_guard_t *g=NULL;
    cog_result_t rc=cog_store_owner_acquire(s,"process",(cog_store_owner_t){1U,101U},&g);
    if(!hold){cog_store_free(s);return rc==COG_ERROR_AGAIN ? 0 : 2;}
    CHECK(rc==COG_OK);
    cog_session_t *conversation=session("process");cog_session_node_t *n=proposal(conversation);
    CHECK(cog_session_tool_transition(conversation,n,"call",COG_TOOL_EXECUTION_PROPOSED,COG_TOOL_EXECUTION_STARTED)==COG_OK);
    CHECK(cog_store_save(s,conversation)==COG_OK);
    char ready='R';CHECK(write(STDOUT_FILENO,&ready,1U)==1);
    /* The parent kills this independently exec'd owner after durable STARTED. */
    for(;;)pause();
}
static void exec_probe(const char *exe,const char *base)
{
    pid_t pid=fork();CHECK(pid>=0);
    if(pid==0){execl(exe,exe,"probe",base,(char *)NULL);_exit(127);}
    int status;CHECK(waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
static void process_crash(const char *exe,const char *base)
{
    cog_store_t *s=store(base,true);cog_store_checkpoint_t cp={0};
    cog_store_owner_t one={1U,101U},two={2U,202U},three={3U,303U};
    CHECK(cog_store_owner_compare_exchange(s,"process",(cog_store_owner_t){0},one,cp)==COG_OK);
    cog_store_owner_guard_t *g=NULL;CHECK(cog_store_owner_acquire(s,"process",one,&g)==COG_OK);
    char anchor[1024];CHECK(snprintf(anchor,sizeof(anchor),"%s/process/.owner.lock",base)>0);
    struct stat before,after;CHECK(stat(anchor,&before)==0);
    exec_probe(exe,base);CHECK(cog_store_delete(s,"process")==COG_ERROR_UNSUPPORTED);
    CHECK(cog_store_owner_release(g)==COG_OK);
    int pipefd[2];CHECK(pipe(pipefd)==0);pid_t pid=fork();CHECK(pid>=0);
    if(pid==0){CHECK(close(pipefd[0])==0);CHECK(dup2(pipefd[1],STDOUT_FILENO)==STDOUT_FILENO);CHECK(close(pipefd[1])==0);
        execl(exe,exe,"hold",base,(char *)NULL);_exit(127);}
    CHECK(close(pipefd[1])==0);char ready=0;CHECK(read(pipefd[0],&ready,1U)==1 && ready=='R');CHECK(close(pipefd[0])==0);
    CHECK(cog_store_owner_compare_exchange(s,"process",one,two,(cog_store_checkpoint_t){1U})==COG_ERROR_AGAIN);
    CHECK(kill(pid,SIGKILL)==0);int status;CHECK(waitpid(pid,&status,0)==pid && WIFSIGNALED(status));
    /* Reap/fence completed. Fresh process incarnation can now admit recovery. */
    CHECK(cog_store_owner_read(s,"process",&one,&cp)==COG_OK && cp.generation==1U);
    CHECK(cog_store_owner_compare_exchange(s,"process",one,two,cp)==COG_OK);
    CHECK(cog_store_owner_acquire(s,"process",two,&g)==COG_ERROR_INDETERMINATE && g==NULL);
    CHECK(cog_store_owner_acquire_recovery(s,"process",two,&g)==COG_OK);
    cog_session_t *conversation=NULL;CHECK(cog_store_load(s,"process",&conversation)==COG_OK);
    cog_message_t r={.role=COG_MESSAGE_ROLE_TOOL,.tool_call_id="call",.content="unknown effect acknowledged",.content_len=27U};
    CHECK(cog_session_reconcile_tool(conversation,cog_session_root(conversation),"call",COG_TOOL_EXECUTION_ACKNOWLEDGED,&r,NULL)==COG_OK);
    CHECK(cog_store_save(s,conversation)==COG_OK);CHECK(cog_store_owner_read(s,"process",&two,&cp)==COG_OK);
    CHECK(cog_store_owner_release(g)==COG_OK);CHECK(cog_store_owner_compare_exchange(s,"process",two,three,cp)==COG_OK);
    CHECK(cog_store_owner_acquire(s,"process",three,&g)==COG_OK);CHECK(cog_store_owner_release(g)==COG_OK);
    CHECK(stat(anchor,&after)==0 && before.st_dev==after.st_dev && before.st_ino==after.st_ino);
    CHECK(cog_store_delete(s,"process")==COG_ERROR_UNSUPPORTED);
    cog_session_free(conversation);cog_store_free(s);
}
static void save_faults(bool crash)
{
    /* Each intercepted boundary is a real write/fsync/link/rename operation.
     * Restart must expose exactly the old or new complete transaction, never a
     * partial generation; immutable checkpoint 1 remains byte-valid throughout. */
    unsigned total=0U;
    for(unsigned at=1U;at<64U;++at){
        char base[]="/tmp/cog-store-publish-XXXXXX";CHECK(mkdtemp(base)!=NULL);
        cog_store_t *s=store(base,false);cog_session_t *c=session("transaction");
        cog_session_node_t *assistant=proposal(c);
        CHECK(cog_session_tool_transition(c,assistant,"call",COG_TOOL_EXECUTION_PROPOSED,COG_TOOL_EXECUTION_STARTED)==COG_OK);
        CHECK(cog_store_save(s,c)==COG_OK);
        CHECK(cog_session_tool_transition(c,assistant,"call",COG_TOOL_EXECUTION_STARTED,COG_TOOL_EXECUTION_SUCCEEDED)==COG_OK);
        const cog_message_t result={.role=COG_MESSAGE_ROLE_TOOL,.tool_call_id="call",.content="one effect",.content_len=10U};
        CHECK(cog_session_append(c,&result,NULL)==COG_OK);
        if(crash){
            pid_t pid=fork();CHECK(pid>=0);
            if(pid==0){boundary=0U;trigger=at;crash_fault=true;enabled=true;cog_result_t rc=cog_store_save(s,c);enabled=false;_exit(rc==COG_OK?0:3);}
            int status;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status));
            CHECK(WEXITSTATUS(status)==0 || WEXITSTATUS(status)==77);
            if(WEXITSTATUS(status)==0)total=at-1U;
        }else{
            boundary=0U;trigger=at;crash_fault=false;enabled=true;cog_result_t rc=cog_store_save(s,c);enabled=false;
            CHECK(rc==COG_OK || rc==COG_ERROR_IO);if(rc==COG_OK)total=boundary;
        }
        cog_store_free(s);s=store(base,false);cog_session_t *out=NULL;cog_store_load_report_t r;
        CHECK(cog_store_load_ex(s,"transaction",COG_STORE_LOAD_REPLAY,&out,&r)==COG_OK);
        cog_session_snapshot_t v;CHECK(cog_session_snapshot_view(out,&v)==COG_OK);
        CHECK(v.node_count==1U || v.node_count==2U);CHECK(v.ledger_count==1U);
        if(v.node_count==1U)CHECK(v.ledger[0].state==COG_TOOL_EXECUTION_INDETERMINATE);
        else CHECK(v.ledger[0].state==COG_TOOL_EXECUTION_SUCCEEDED && strcmp(v.nodes[1].content,"one effect")==0);
        cog_session_free(out);out=NULL;
        CHECK(cog_store_checkpoint_load(s,"transaction",(cog_store_checkpoint_t){1U},COG_STORE_LOAD_REPLAY,&out,&r)==COG_OK);
        CHECK(cog_session_snapshot_view(out,&v)==COG_OK && v.node_count==1U &&
            v.ledger[0].state==COG_TOOL_EXECUTION_INDETERMINATE);cog_session_free(out);
        /* Failed attempts do not make an earlier immutable token reusable. */
        CHECK(cog_store_save(s,c)==COG_OK);
        cog_store_free(s);cog_session_free(c);cleanup(base);
        if(total!=0U)break;
    }
    CHECK(total>=10U);(void)printf("%s: %u transaction syscall boundaries\n",crash?"crash":"EIO",total);
}
static void cas_faults(bool crash)
{
    unsigned total=0U;
    for(unsigned at=1U;at<32U;++at){
        char base[]="/tmp/cog-store-owner-XXXXXX";CHECK(mkdtemp(base)!=NULL);
        cog_store_t *s=store(base,true);cog_store_owner_t zero={0},one={1U,111U},two={2U,222U};cog_store_checkpoint_t cp={0};
        CHECK(cog_store_owner_compare_exchange(s,"owner",zero,one,cp)==COG_OK);
        if(crash){
            pid_t pid=fork();CHECK(pid>=0);
            if(pid==0){boundary=0U;trigger=at;crash_fault=true;enabled=true;
                cog_result_t rc=cog_store_owner_compare_exchange(s,"owner",one,two,cp);enabled=false;_exit(rc==COG_OK?0:3);}
            int status;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status));
            CHECK(WEXITSTATUS(status)==0 || WEXITSTATUS(status)==77);if(WEXITSTATUS(status)==0)total=at-1U;
        }else{
            boundary=0U;trigger=at;crash_fault=false;enabled=true;
            cog_result_t rc=cog_store_owner_compare_exchange(s,"owner",one,two,cp);enabled=false;
            CHECK(rc==COG_OK || rc==COG_ERROR_IO || rc==COG_ERROR_INDETERMINATE);if(rc==COG_OK)total=boundary;
        }
        cog_store_free(s);s=store(base,true);cog_store_owner_t actual;
        CHECK(cog_store_owner_read(s,"owner",&actual,&cp)==COG_OK && (actual.epoch==1U || actual.epoch==2U));
        /* Lost ACK or final directory fsync failure resolves by identical retry,
         * which revalidates and fsync-confirms instead of guessing rollback. */
        CHECK(cog_store_owner_compare_exchange(s,"owner",one,two,cp)==COG_OK);
        cog_store_owner_guard_t *g=NULL;CHECK(cog_store_owner_acquire(s,"owner",two,&g)==COG_OK);CHECK(cog_store_owner_release(g)==COG_OK);
        cog_store_free(s);cleanup(base);if(total!=0U)break;
    }
    CHECK(total>=8U);(void)printf("owner %s: %u syscall boundaries\n",crash?"crash":"EIO",total);
}
static void stale_temp_checkpoint(void)
{
    char base[]="/tmp/cog-store-stale-temp-XXXXXX";CHECK(mkdtemp(base)!=NULL);
    cog_store_t *s=store(base,false);cog_session_t *c=session("stale");
    CHECK(cog_store_save(s,c)==COG_OK);
    char generation[1024],temporary[1024];
    CHECK(snprintf(generation,sizeof(generation),"%s/stale/generation-%020u.bin",base,1U)>0);
    CHECK(snprintf(temporary,sizeof(temporary),"%s/stale/.generation.tmp",base)>0);
    /* Reproduce an old writer's ignored unlink failure: the temporary name
     * still aliases an already committed immutable inode. It must never be
     * opened with O_TRUNC, even if unlink fails again on the next save. */
    CHECK(link(generation,temporary)==0);
    const cog_message_t m={.role=COG_MESSAGE_ROLE_USER,.content="later",.content_len=5U};
    CHECK(cog_session_append(c,&m,NULL)==COG_OK);fail_temp_unlink=true;
    CHECK(cog_store_save(s,c)==COG_ERROR_IO);
    cog_session_t *out=NULL;cog_store_load_report_t r;
    CHECK(cog_store_checkpoint_load(s,"stale",(cog_store_checkpoint_t){1U},COG_STORE_LOAD_REPLAY,&out,&r)==COG_OK);
    CHECK(cog_session_is_empty(out));cog_session_free(out);
    CHECK(cog_store_save(s,c)==COG_OK);
    CHECK(cog_store_checkpoint_load(s,"stale",(cog_store_checkpoint_t){1U},COG_STORE_LOAD_REPLAY,&out,&r)==COG_OK);
    CHECK(cog_session_is_empty(out));cog_session_free(out);cog_session_free(c);cog_store_free(s);cleanup(base);
}
static void session_limit_race(void)
{
    char base[]="/tmp/cog-store-limit-race-XXXXXX";CHECK(mkdtemp(base)!=NULL);
    cog_store_config_t cfg={.base_dir=base,.max_sessions=1U,.max_record_bytes=1048576U};
    cog_store_t *first=NULL,*second=NULL;CHECK(cog_store_new(&first,&cfg)==COG_OK && cog_store_new(&second,&cfg)==COG_OK);
    cog_session_t *a=session("first"),*b=session("second");
    int ready[2],release[2];CHECK(pipe(ready)==0 && pipe(release)==0);pid_t pid=fork();CHECK(pid>=0);
    if(pid==0){pause_ready_fd=ready[1];pause_release_fd=release[0];_exit(cog_store_save(first,a)==COG_OK?0:3);}
    CHECK(close(ready[1])==0 && close(release[0])==0);char byte;
    CHECK(read(ready[0],&byte,1U)==1);
    /* First session holds global admission through immutable installation and
     * manifest durability. A distinct session cannot pass its count check. */
    CHECK(cog_store_save(second,b)==COG_ERROR_AGAIN);
    CHECK(write(release[1],"R",1U)==1);int status;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(cog_store_save(second,b)==COG_ERROR_LIMIT);
    CHECK(close(ready[0])==0 && close(release[1])==0);
    char **ids=NULL;size_t count=0U;CHECK(cog_store_list(first,&ids,&count)==COG_OK && count==1U);
    free(ids[0]);free(ids);cog_session_free(a);cog_session_free(b);cog_store_free(first);cog_store_free(second);cleanup(base);
}
static void bounded_allocations(void)
{
    char base[]="/tmp/cog-store-allocation-XXXXXX";CHECK(mkdtemp(base)!=NULL);
    cog_store_config_t cfg={.base_dir=base,.require_owner=true,.max_record_bytes=1048576U};
    size_t count=0U;
    for(size_t at=1U;at<16U;++at){
        allocation_attempts=0U;allocation_failure=at;cog_store_t *s=NULL;
        cog_result_t rc=cog_store_new(&s,&cfg);allocation_failure=0U;
        if(rc==COG_OK){count=allocation_attempts;cog_store_free(s);break;}
        CHECK(rc==COG_ERROR_NOMEM && s==NULL);
    }
    CHECK(count==6U);
    cog_store_t *s=store(base,true);cog_session_t *c=session("bounded");
    cog_store_owner_t owner={1U,123U};cog_store_checkpoint_t cp={0};
    allocation_attempts=0U;reject_allocations=true;
    CHECK(cog_store_owner_compare_exchange(s,"bounded",(cog_store_owner_t){0},owner,cp)==COG_OK);
    CHECK(allocation_attempts==0U);reject_allocations=false;
    cog_store_owner_guard_t *g=NULL;
    reject_allocations=true;CHECK(cog_store_owner_acquire(s,"bounded",owner,&g)==COG_ERROR_NOMEM && g==NULL);reject_allocations=false;
    CHECK(cog_store_owner_acquire(s,"bounded",owner,&g)==COG_OK);
    allocation_attempts=0U;reject_allocations=true;
    CHECK(cog_store_save(s,c)==COG_OK);
    CHECK(cog_store_owner_read(s,"bounded",&owner,&cp)==COG_OK);
    CHECK(cog_store_checkpoint_save(s,c,g,&cp)==COG_OK);
    CHECK(cog_store_owner_guard_borrow(g,"bounded")==COG_OK);CHECK(cog_store_owner_guard_return(g)==COG_OK);
    CHECK(cog_store_owner_release(g)==COG_OK);
    CHECK(cog_store_owner_compare_exchange(s,"bounded",owner,(cog_store_owner_t){2U,456U},cp)==COG_OK);
    CHECK(allocation_attempts==0U);reject_allocations=false;
    cog_session_free(c);cog_store_free(s);cleanup(base);
}
int main(int argc,char **argv)
{
    if(argc==3)return child_probe(argv[2],strcmp(argv[1],"hold")==0);
    char exe[1024];ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1U);CHECK(n>0);exe[(size_t)n]='\0';
    char base[]="/tmp/cog-store-process-XXXXXX";CHECK(mkdtemp(base)!=NULL);
    process_crash(exe,base);cleanup(base);bounded_allocations();session_limit_race();stale_temp_checkpoint();save_faults(false);save_faults(true);cas_faults(false);cas_faults(true);
    (void)puts("test_store_fencing: ok");return 0;
}
