#define _POSIX_C_SOURCE 200809L
#include "../src/fixture_provider.h"
#include "../src/execution_internal.h"
#include "../src/runtime_internal.h"
#include <assert.h>
#include <cog/runtime.h>
#include <cog/runtime_instructions.h>
#include <cog/openrouter.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <cog/workspace.h>

static bool contains(const char *text,size_t bytes,const char *needle) {
  size_t n=strlen(needle);
  for(size_t i=0U;n<=bytes && i<=bytes-n;i++)if(memcmp(text+i,needle,n)==0)return true;
  return false;
}
static cog_runtime_config_t config(const cog_endpoint_t *endpoint) {
  cog_runtime_config_t c = {0};
  c.mode = COG_RUNTIME_NEW;
  c.instruction_mode = COG_RUNTIME_INSTRUCTIONS_DISABLED;
  c.session_id = "runtime-test";
  c.store.backend = &cog_store_null_backend;
  c.session.max_nodes = 32U;
  c.session.max_payload_bytes = 262144U;
  c.session.max_tool_calls_per_message = 4U;
  c.provider.endpoint = endpoint;
  c.provider.model_id = "fixture";
  c.limits = (cog_runtime_limits_t){.max_input_bytes = 4096U,
                                    .max_queued_inputs = 4U,
                                    .max_view_bytes = 1048576U,
                                    .max_context_bytes = 65536U,
                                    .max_tool_output_bytes = 4096U,
                                    .max_instruction_bytes = 65536U,
                                    .max_events = 64U,
                                    .max_dispatch_per_step = 1U,
                                    .max_output_tokens = 128U,
                                    .max_tool_rounds = 4U,
                                    .max_turn_ms = 10000U,
                                    .max_tool_ms = 1000U,
                                    .max_retries = 0U,
                                    .max_retry_elapsed_ms = 1000U};
  return c;
}
static void stream_without_terminal(void) {
  static const cog_mock_step_t steps[] = {
      {.kind = COG_MOCK_STEP_CHUNK, .chunk = "headless output"}};
  static const cog_mock_script_t script = {
      .steps = steps, .step_count = 1U, .repeat = 1U};
  static const cog_model_t model = {.id = "fixture",
                                    .name = "fixture",
                                    .capabilities =
                                        COG_MODEL_CAPABILITY_STREAMING};
  static const cog_model_table_t models = {&model, 1U};
  cog_endpoint_t endpoint = {.name = "fixture",
                             .vtable = &cog_mock_provider_vtable,
                             .models = &models,
                             .implementation_config = &script,
                             .generation = 1U};
  cog_runtime_config_t c = config(&endpoint);
  cog_runtime_t *r = NULL;
  int incoming[2], outgoing[2];
  assert(pipe(incoming) == 0 && pipe(outgoing) == 0);
  int saved_in = dup(STDIN_FILENO), saved_out = dup(STDOUT_FILENO),
      saved_error = dup(STDERR_FILENO);
  assert(saved_in >= 0 && saved_out >= 0 && saved_error >= 0);
  assert(write(incoming[1], "untouched", 9U) == 9);
  assert(dup2(incoming[0], STDIN_FILENO) >= 0);
  assert(dup2(outgoing[1], STDOUT_FILENO) >= 0);
  assert(dup2(outgoing[1], STDERR_FILENO) >= 0);
  assert(cog_runtime_new(&r, &c) == COG_OK);
  assert(cog_runtime_begin(r) == COG_OK);
  bool submitted = false, saw_output = false, stopping = false, done = false;
  for (size_t i = 0U; i < 10000U && !done; i++) {
    cog_runtime_view_t view;
    cog_result_t rc = cog_runtime_view(r, &view);
    if (rc == COG_OK) {
      for (size_t j = 0U; j < view.item_count; j++)
        if (view.items[j].role == COG_MESSAGE_ROLE_ASSISTANT &&
            view.items[j].text_bytes == 15U &&
            memcmp(view.items[j].text, "headless output", 15U) == 0)
          saw_output = true;
      assert(cog_runtime_ack(r, view.serial) == COG_OK);
      assert(cog_runtime_ack(r, view.serial) == COG_OK);
      if (!submitted) {
        assert(cog_runtime_submit(r, "hello", 5U, false) == COG_OK);
        submitted = true;
      }
    } else
      assert(rc == COG_ERROR_AGAIN);
    if (saw_output && !stopping) {
      assert(cog_runtime_shutdown(r) == COG_OK);
      stopping = true;
    }
    cog_runtime_step_t step;
    rc = cog_runtime_step(r, &step);
    assert(rc == COG_OK || rc == COG_ERROR_AGAIN);
    done = step.done;
  }
  assert(done && submitted && saw_output);
  assert(cog_runtime_close(&r, COG_OK) == COG_OK && r == NULL);
  char text[16] = {0};
  assert(read(STDIN_FILENO, text, 9U) == 9 &&
         memcmp(text, "untouched", 9U) == 0);
  assert(dup2(saved_in, STDIN_FILENO) >= 0 &&
         dup2(saved_out, STDOUT_FILENO) >= 0 &&
         dup2(saved_error, STDERR_FILENO) >= 0);
  close(saved_in);
  close(saved_out);
  close(saved_error);
  close(outgoing[1]);
  assert(read(outgoing[0], text, sizeof(text)) == 0);
  close(incoming[0]);
  close(incoming[1]);
  close(outgoing[0]);
}
static void close_before_begin(void) {
  static const cog_mock_script_t script = {0};
  static const cog_model_t model = {.id = "fixture"};
  static const cog_model_table_t models = {&model, 1U};
  cog_endpoint_t endpoint = {.name = "fixture",
                             .vtable = &cog_mock_provider_vtable,
                             .models = &models,
                             .implementation_config = &script,
                             .generation = 1U};
  cog_runtime_config_t c = config(&endpoint);
  cog_runtime_t *r = NULL;
  assert(cog_runtime_new(&r, &c) == COG_OK);
  assert(cog_runtime_shutdown(r) == COG_OK);
  assert(cog_runtime_begin(r) == COG_ERROR_INVALID_STATE);
  assert(cog_runtime_close(&r, COG_OK) == COG_OK && r == NULL);
  c.limits.max_queued_inputs = 9U;
  assert(cog_runtime_new(&r, &c) == COG_ERROR_LIMIT && r == NULL);
}
static void pinned_resume(void) {
  static const cog_mock_script_t script = {0};
  static const cog_model_t model = {.id = "fixture"};
  static const cog_model_table_t models = {&model, 1U};
  cog_endpoint_t endpoint = {.name = "fixture", .vtable = &cog_mock_provider_vtable,
      .models = &models, .implementation_config = &script, .generation = 1U};
  cog_provider_prompt_template_t templ = cog_openrouter_prompt_template;
  templ.provider = endpoint.vtable;
  cog_runtime_config_t c = config(&endpoint);
  char directory[] = "runtime-store-XXXXXX";
  assert(mkdtemp(directory) != NULL);
  c.store.backend = &cog_store_file_backend;
  c.store.base_dir = directory;
  c.store.require_owner = true;
  c.instruction_mode = COG_RUNTIME_INSTRUCTIONS_PINNED;
  c.workspace_root_id = "root";
  c.session.max_contexts = 8U;
  c.session.max_context_bindings = 16U;
  c.session.max_context_bytes = 262144U;
  c.instruction_limits = (cog_instruction_limits_t){32,16,4096,8192,4096,131072,64,16,16,16};
  c.provider.prompt_template = &templ;
  c.provider.compiler_limits = (cog_provider_prompt_limits_t){64,64,16,32,65536,262144,65536};
  cog_instruction_source_t source = {COG_INSTRUCTION_HOST,"root","host.md","",NULL,0,"RULE",4};
  cog_instruction_catalog_t *catalog = NULL;
  assert(cog_instruction_catalog_new(&catalog,&c.instruction_limits,1U,&source,1U,NULL)==COG_OK);
  cog_instruction_selection_t selection = {.snapshot_id=1U,.workspace_root_id="root",.scope="",
      .model_id="fixture",.template_id=templ.id,.template_revision=templ.revision,
      .max_context_bytes=65536,.max_output_tokens=128,.max_tool_rounds=4};
  cog_instruction_snapshot_t *first=NULL,*next=NULL;
  assert(cog_instruction_snapshot_new(&first,catalog,&c.instruction_limits,&selection,NULL,0U,NULL)==COG_OK);
  selection.snapshot_id=2U;
  assert(cog_instruction_snapshot_new(&next,catalog,&c.instruction_limits,&selection,NULL,0U,NULL)==COG_OK);
  c.instructions=first;
  cog_runtime_t *r=NULL;
  /* Invalid template identity must fail before creating a durable session. */
  templ.revision++;
  assert(cog_runtime_new(&r,&c)!=COG_OK && r==NULL);
  templ.revision--;
  cog_store_t *inspection=NULL;
  assert(cog_store_new(&inspection,&c.store)==COG_OK);
  assert(!cog_store_exists(inspection,c.session_id));
  cog_store_free(inspection);
  assert(cog_runtime_new(&r,&c)==COG_OK);
  cog_runtime_instruction_change_t *change=NULL;
  /* Host maxima do not authorize increasing a currently reduced snapshot. */
  cog_instruction_snapshot_t *reduced=NULL;
  selection.snapshot_id=3U;selection.max_output_tokens=64U;
  assert(cog_instruction_snapshot_new(&reduced,catalog,&c.instruction_limits,&selection,NULL,0U,NULL)==COG_OK);
  assert(cog_runtime_instruction_prepare(&change,r,next)==COG_OK);
  assert(cog_runtime_instruction_change_free(&change)==COG_OK);
  assert(cog_runtime_instruction_prepare(&change,r,next)==COG_ERROR_INVALID_ARG);
  assert(cog_runtime_close(&r,COG_OK)==COG_OK);
  c.mode=COG_RUNTIME_RESUME;
  c.instructions=NULL;
  assert(cog_runtime_new(&r,&c)==COG_OK);
  assert(cog_runtime_begin(r)==COG_OK);
  cog_runtime_view_t view;
  assert(cog_runtime_view(r,&view)==COG_OK);
  assert(view.instruction_snapshot_id==1U);
  assert(cog_runtime_ack(r,view.serial)==COG_OK);
  assert(cog_runtime_instruction_prepare(&change,r,next)==COG_OK);
  assert(cog_runtime_instruction_publish(r,&change)==COG_OK && change==NULL);
  assert(cog_runtime_view(r,&view)==COG_OK && view.instruction_snapshot_id==2U);
  assert(cog_runtime_ack(r,view.serial)==COG_OK);
  cog_endpoint_t replacement=endpoint;replacement.generation=2U;
  cog_runtime_provider_config_t paired=c.provider;paired.endpoint=&replacement;
  assert(cog_runtime_reload(r,&paired,reduced)==COG_OK);
  bool pair_visible=false;
  for(size_t i=0U;i<1000U && !pair_visible;i++) {
    cog_runtime_step_t progress;
    cog_result_t rc=cog_runtime_step(r,&progress);assert(rc==COG_OK || rc==COG_ERROR_AGAIN);
    rc=cog_runtime_view(r,&view);
    if(rc==COG_OK) {
      pair_visible=view.active_generation==2U && view.instruction_snapshot_id==3U;
      assert(cog_runtime_ack(r,view.serial)==COG_OK);
    } else assert(rc==COG_ERROR_AGAIN);
  }
  assert(pair_visible);
  assert(cog_runtime_shutdown(r)==COG_OK);
  cog_runtime_step_t step={0};
  for(size_t i=0U;i<1000U && !step.done;i++) {
    cog_result_t rc=cog_runtime_step(r,&step);
    assert(rc==COG_OK || rc==COG_ERROR_AGAIN);
  }
  assert(step.done);
  assert(cog_runtime_close(&r,COG_OK)==COG_OK);
  c.mode=COG_RUNTIME_NEW;c.session_id="reduced-test";c.instructions=reduced;
  assert(cog_runtime_new(&r,&c)==COG_OK);
  selection.snapshot_id=4U;selection.max_output_tokens=128U;
  cog_instruction_snapshot_t *broader=NULL;
  assert(cog_instruction_snapshot_new(&broader,catalog,&c.instruction_limits,&selection,NULL,0U,NULL)==COG_OK);
  assert(cog_runtime_instruction_prepare(&change,r,broader)==COG_ERROR_DENIED && change==NULL);
  assert(cog_runtime_close(&r,COG_OK)==COG_OK);
  c.mode=COG_RUNTIME_INSPECT;c.session_id="runtime-test";c.instructions=NULL;
  c.provider=(cog_runtime_provider_config_t){0};
  assert(cog_runtime_new(&r,&c)==COG_OK);
  assert(cog_runtime_view(r,&view)==COG_OK && view.instruction_snapshot_id==3U);
  assert(cog_runtime_begin(r)==COG_ERROR_INVALID_STATE);
  assert(cog_runtime_close(&r,COG_OK)==COG_OK);
  cog_instruction_snapshot_free(reduced);cog_instruction_snapshot_free(broader);
  cog_instruction_snapshot_free(first);
  cog_instruction_snapshot_free(next);
  cog_instruction_catalog_free(catalog);
}
static void coordinated_close(void) {
  static const cog_mock_script_t script={0};
  static const cog_model_t model={.id="fixture"};
  static const cog_model_table_t models={&model,1U};
  cog_endpoint_t endpoint={.name="fixture",.vtable=&cog_mock_provider_vtable,
      .models=&models,.implementation_config=&script,.generation=7U};
  cog_runtime_config_t c=config(&endpoint);
  char directory[]="runtime-host-XXXXXX";assert(mkdtemp(directory)!=NULL);
  c.store.backend=&cog_store_file_backend;c.store.base_dir=directory;c.store.require_owner=true;
  cog_prompt_tool_binding_t binding={&cog_runtime_delegation_tools.tools[0],COG_PROMPT_TOOL_ALLOW};
  c.tools.bindings=&binding;c.tools.binding_count=1U;
  cog_runtime_t *r=NULL;
  assert(cog_runtime_new(&r,&c)==COG_OK);
  assert(cog_runtime_begin(r)==COG_OK);
  cog_runtime_view_t view;
  cog_runtime_step_t initial;
  cog_result_t initial_rc=cog_runtime_step(r,&initial);
  assert(initial_rc==COG_OK || initial_rc==COG_ERROR_AGAIN);
  assert(cog_runtime_view(r,&view)==COG_OK && view.active_generation==7U);
  assert(cog_runtime_ack(r,view.serial)==COG_OK);
  cog_endpoint_t replacement=endpoint;replacement.generation=8U;
  cog_runtime_provider_config_t next_provider=c.provider;next_provider.endpoint=&replacement;
  assert(cog_runtime_reload(r,&next_provider,NULL)==COG_OK);
  assert(cog_runtime_submit(r,"blocked",7U,false)==COG_ERROR_AGAIN);
  bool reloaded=false;
  for(size_t i=0U;i<1000U && !reloaded;i++) {
    cog_runtime_step_t progress;
    cog_result_t rc=cog_runtime_step(r,&progress);assert(rc==COG_OK || rc==COG_ERROR_AGAIN);
    rc=cog_runtime_view(r,&view);
    if(rc==COG_OK) {
      reloaded=view.active_generation==8U && view.published_generation==8U;
      assert(cog_runtime_ack(r,view.serial)==COG_OK);
    } else assert(rc==COG_ERROR_AGAIN);
  }
  assert(reloaded);
  assert(cog_runtime_shutdown(r)==COG_OK);
  cog_runtime_step_t step={0};
  for(size_t i=0U;i<1000U && !step.done;i++) {
    cog_result_t rc=cog_runtime_step(r,&step);assert(rc==COG_OK || rc==COG_ERROR_AGAIN);
  }
  assert(step.done);
  assert(cog_runtime_close(&r,COG_OK)==COG_OK && r==NULL);
}
typedef struct typed_fixture { unsigned connections; bool named, control; } typed_fixture_t;
static cog_result_t typed_connect(const cog_endpoint_t *endpoint,cog_provider_t **out) {
  typed_fixture_t *fixture=(typed_fixture_t *)endpoint->implementation_config;
  static const char arguments[]="{\"tasks\":[{\"task\":\"child task\"}]}";
  static const cog_provider_delta_t parent[]={
    {.kind=COG_PROVIDER_DELTA_TOOL_BEGIN,.tool_call_id="delegation",.tool_name="delegate"},
    {.kind=COG_PROVIDER_DELTA_TOOL_ARGUMENTS,.tool_call_id="delegation",.data=arguments,.data_len=sizeof(arguments)-1U},
    {.kind=COG_PROVIDER_DELTA_TOOL_END,.tool_call_id="delegation"},
    {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_TOOL_CALLS},
    {.kind=COG_PROVIDER_DELTA_TEXT,.data="parent done",.data_len=11U},
    {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_STOP}};
  static const cog_provider_delta_t child[]={
    {.kind=COG_PROVIDER_DELTA_TEXT,.data="child done",.data_len=10U},
    {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_STOP}};
  bool first=fixture->connections++==0U;
  static const char named_arguments[]="{\"tasks\":[{\"task\":\"child task\",\"agent\":\"reader\"}]}";
  static const cog_provider_delta_t named_parent[]={
    {.kind=COG_PROVIDER_DELTA_TOOL_BEGIN,.tool_call_id="delegation",.tool_name="delegate"},
    {.kind=COG_PROVIDER_DELTA_TOOL_ARGUMENTS,.tool_call_id="delegation",.data=named_arguments,.data_len=sizeof(named_arguments)-1U},
    {.kind=COG_PROVIDER_DELTA_TOOL_END,.tool_call_id="delegation"},
    {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_TOOL_CALLS},
    {.kind=COG_PROVIDER_DELTA_TEXT,.data="parent done",.data_len=11U},
    {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_STOP}};
  static const char activation[]="{\"name\":\"test\"}";
  static const cog_provider_delta_t control_parent[]={
    {.kind=COG_PROVIDER_DELTA_TOOL_BEGIN,.tool_call_id="activation",.tool_name="activate_skill"},
    {.kind=COG_PROVIDER_DELTA_TOOL_ARGUMENTS,.tool_call_id="activation",.data=activation,.data_len=sizeof(activation)-1U},
    {.kind=COG_PROVIDER_DELTA_TOOL_END,.tool_call_id="activation"},
    {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_TOOL_CALLS},
    {.kind=COG_PROVIDER_DELTA_TEXT,.data="parent done",.data_len=11U},
    {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_STOP}};
  return cog_mock_provider_new_deltas(first ? (fixture->control ? control_parent : fixture->named ? named_parent : parent) : child,
      first ? sizeof(parent)/sizeof(parent[0]) : sizeof(child)/sizeof(child[0]),out);
}
static const cog_provider_vtable_t typed_table={typed_connect,cog_provider_disconnect,
    cog_provider_send,cog_provider_receive,cog_provider_poll,cog_provider_cancel,
    cog_provider_next,cog_provider_get_error,cog_provider_preflight};
static void local_delegation(bool pinned,bool control) {
  typed_fixture_t fixture={.named=pinned,.control=control};
  static const cog_model_t model={.id="fixture",.capabilities=COG_MODEL_CAPABILITY_STREAMING|COG_MODEL_CAPABILITY_TOOLS};
  static const cog_model_table_t models={&model,1U};
  cog_endpoint_t endpoint={.name="fixture",.vtable=&typed_table,.models=&models,
      .implementation_config=&fixture,.generation=1U};
  cog_runtime_config_t c=config(&endpoint);
  char directory[]="runtime-delegate-XXXXXX";assert(mkdtemp(directory)!=NULL);
  c.store.backend=&cog_store_file_backend;c.store.base_dir=directory;c.store.require_owner=true;
  cog_prompt_tool_binding_t binding={control ? &cog_runtime_activate_skill_tool : &cog_runtime_delegation_tools.tools[0],control ? COG_PROMPT_TOOL_ASK : COG_PROMPT_TOOL_ALLOW};
  c.tools.bindings=&binding;c.tools.binding_count=1U;
  cog_instruction_catalog_t *catalog=NULL;
  cog_instruction_snapshot_t *snapshot=NULL,*denied_snapshot=NULL;
  cog_provider_prompt_template_t templ=cog_openrouter_prompt_template;templ.provider=&typed_table;
  c.provider.prompt_template=&templ;
  c.provider.compiler_limits=(cog_provider_prompt_limits_t){64,64,16,32,65536,262144,65536};
  if(pinned) {
    c.instruction_mode=COG_RUNTIME_INSTRUCTIONS_PINNED;c.workspace_root_id="root";
    c.session.max_contexts=8U;c.session.max_context_bindings=16U;c.session.max_context_bytes=262144U;
    c.instruction_limits=(cog_instruction_limits_t){32,16,4096,8192,4096,131072,64,16,16,16};
    c.provider.prompt_template=&templ;
    c.provider.compiler_limits=(cog_provider_prompt_limits_t){64,64,16,32,65536,262144,65536};
    static const char role[]="---\ncog-agent: 1\nname: reader\ndescription: Read only\ntools: ''\n---\nCHILD ROLE\n";
    static const char skill[]="---\nname: test\ndescription: Activate test\n---\nSKILL SECRET BODY\n";
    cog_instruction_source_t sources[]={
      {COG_INSTRUCTION_HOST,"root","host.md","",NULL,0,"HOST",4},
      {COG_INSTRUCTION_AGENT,"root","agents/reader.md","",NULL,0,role,sizeof(role)-1U}};
    if(control)sources[1]=(cog_instruction_source_t){COG_INSTRUCTION_SKILL,"root","skills/test/SKILL.md","",NULL,0,skill,sizeof(skill)-1U};
    assert(cog_instruction_catalog_new(&catalog,&c.instruction_limits,1U,sources,2U,NULL)==COG_OK);
    cog_instruction_selection_t selection={.snapshot_id=1U,.workspace_root_id="root",.scope="",
      .model_id="fixture",.template_id=templ.id,.template_revision=templ.revision,.max_context_bytes=65536,
      .max_output_tokens=128,.max_tool_rounds=4,.bindings=&binding,.binding_count=1U};
    assert(cog_instruction_snapshot_new(&snapshot,catalog,&c.instruction_limits,&selection,NULL,0U,NULL)==COG_OK);
    c.instructions=snapshot;
    if(!control) {
      cog_prompt_tool_binding_t denied=binding;denied.policy=COG_PROMPT_TOOL_DENY;
      selection.bindings=&denied;selection.snapshot_id=2U;
      assert(cog_instruction_snapshot_new(&denied_snapshot,catalog,&c.instruction_limits,&selection,NULL,0U,NULL)==COG_OK);
    }
  }
  cog_runtime_t *r=NULL;assert(cog_runtime_new(&r,&c)==COG_OK);
  assert(cog_runtime_begin(r)==COG_OK);
  bool submitted=false,complete=false,stopping=false,child_result=false,answered=false,identity_checked=false;
  cog_runtime_step_t step={0};
  for(size_t i=0U;i<10000U && !step.done;i++) {
    cog_runtime_view_t view;cog_result_t rc=cog_runtime_view(r,&view);
    if(rc==COG_OK) {
      for(size_t j=0U;j<view.item_count;j++)
        if(view.items[j].text_bytes==11U && memcmp(view.items[j].text,"parent done",11U)==0)complete=true;
      for(size_t k=0U;k<view.item_count;k++)
        if(view.items[k].role==COG_MESSAGE_ROLE_TOOL && contains(view.items[k].text,view.items[k].text_bytes,control ? "status=ok" : "child done"))child_result=true;
      if(control && complete)assert(view.instruction_snapshot_id==2U);
      assert(cog_runtime_ack(r,view.serial)==COG_OK);
      if(view.policy!=NULL && !answered) {
        cog_runtime_policy_t wrong=*view.policy;wrong.assistant_node_id++;
        assert(cog_runtime_answer(r,&wrong,true)==COG_ERROR_INVALID_STATE);
        assert(cog_runtime_answer(r,view.policy,true)==COG_OK);answered=true;
      }
      if(!submitted){assert(cog_runtime_submit(r,"delegate",8U,false)==COG_OK);submitted=true;}
    } else assert(rc==COG_ERROR_AGAIN);
    if(complete && !stopping){assert(cog_runtime_shutdown(r)==COG_OK);stopping=true;}
    if(pinned && !control && !identity_checked && r->app->assistant!=NULL && r->app->call_count!=0U) {
      cog_tool_execution_state_t state;
      const cog_message_tool_call_t *call=&r->app->call_views[0];
      assert(cog_session_tool_state(r->app->session,r->app->assistant,call->id,&state)==COG_OK);
      if(state==COG_TOOL_EXECUTION_PROPOSED) {
        uint64_t node_id=cog_node_data(r->app->assistant)->node_id;
        cog_tool_t copied=*r->bindings[0].descriptor;
        const cog_tool_t *original=r->bindings[0].descriptor;r->bindings[0].descriptor=&copied;
        assert(cog_runtime_child_select(r,node_id,call->id,0U,"reader",NULL,0U)==COG_ERROR_INVALID_STATE);
        r->bindings[0].descriptor=original;
        cog_instruction_snapshot_t *effective=r->instructions;r->instructions=denied_snapshot;
        assert(cog_runtime_child_select(r,node_id,call->id,0U,"reader",NULL,0U)==COG_ERROR_DENIED);
        r->instructions=effective;identity_checked=true;
      }
    }
    rc=cog_runtime_step(r,&step);assert(rc==COG_OK || rc==COG_ERROR_AGAIN);
  }
  assert(complete && child_result && step.done && fixture.connections==(control ? 1U : 2U));
  assert(!control || answered);
  assert(!pinned || control || identity_checked);
  assert(cog_runtime_close(&r,COG_OK)==COG_OK);
  cog_instruction_snapshot_free(denied_snapshot);cog_instruction_snapshot_free(snapshot);cog_instruction_catalog_free(catalog);
}
static cog_result_t background_connect(const cog_endpoint_t *endpoint, cog_provider_t **out)
{
  (void)endpoint;
  static const char start[] = "{\"command\":\"while test ! -f release; do sleep 0.02; done; printf job-output\"}";
  static const char status[] = "{\"job_id\":\"job-0000000000000001\"}";
  static const cog_provider_delta_t turns[] = {
    {.kind=COG_PROVIDER_DELTA_TOOL_BEGIN,.tool_call_id="launch",.tool_name="start_background"},
    {.kind=COG_PROVIDER_DELTA_TOOL_ARGUMENTS,.tool_call_id="launch",.data=start,.data_len=sizeof(start)-1U},
    {.kind=COG_PROVIDER_DELTA_TOOL_END,.tool_call_id="launch"},
    {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_TOOL_CALLS},
    {.kind=COG_PROVIDER_DELTA_TEXT,.data="job accepted",.data_len=12U},
    {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_STOP},
    {.kind=COG_PROVIDER_DELTA_TEXT,.data="unrelated answer",.data_len=16U},
    {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_STOP},
    {.kind=COG_PROVIDER_DELTA_TOOL_BEGIN,.tool_call_id="lookup",.tool_name="background_status"},
    {.kind=COG_PROVIDER_DELTA_TOOL_ARGUMENTS,.tool_call_id="lookup",.data=status,.data_len=sizeof(status)-1U},
    {.kind=COG_PROVIDER_DELTA_TOOL_END,.tool_call_id="lookup"},
    {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_TOOL_CALLS},
    {.kind=COG_PROVIDER_DELTA_TEXT,.data="job checked",.data_len=11U},
    {.kind=COG_PROVIDER_DELTA_FINISH,.finish_reason=COG_FINISH_REASON_STOP}
  };
  return cog_mock_provider_new_deltas(turns, sizeof(turns)/sizeof(turns[0]), out);
}

static void background_job_round(bool cancel)
{
  static const cog_provider_vtable_t provider = {background_connect,cog_provider_disconnect,
      cog_provider_send,cog_provider_receive,cog_provider_poll,cog_provider_cancel,
      cog_provider_next,cog_provider_get_error,cog_provider_preflight};
  static const cog_model_t model = {.id="fixture",.capabilities=COG_MODEL_CAPABILITY_STREAMING|COG_MODEL_CAPABILITY_TOOLS};
  static const cog_model_table_t models = {&model,1U};
  cog_endpoint_t endpoint = {.name="fixture",.vtable=&provider,.models=&models,.generation=1U};
  cog_runtime_config_t c = config(&endpoint);
  char directory[] = "runtime-job-XXXXXX";
  assert(mkdtemp(directory) != NULL);
  cog_workspace_config_t wc = {.root=directory,.max_file_bytes=4096U,
    .max_search_entries=64U,.service_quantum=4U,.enable_commands=true};
  cog_workspace_t *workspace = NULL;
  assert(cog_workspace_new(&workspace,&wc) == COG_OK);
  const cog_prompt_tool_binding_t bindings[] = {
    {&cog_workspace_start_background_tool,COG_PROMPT_TOOL_ALLOW},
    {&cog_runtime_background_status_tool,COG_PROMPT_TOOL_ALLOW}};
  const cog_runtime_scope_binding_t scope = {&cog_workspace_start_background_tool,"cwd","",false};
  cog_provider_prompt_template_t templ = cog_openrouter_prompt_template;
  templ.provider = &provider;
  c.provider.prompt_template = &templ;
  c.provider.compiler_limits = (cog_provider_prompt_limits_t){64,64,16,32,65536,262144,65536};
  c.store.backend = &cog_store_file_backend;
  c.store.base_dir = directory;
  c.store.require_owner = true;
  c.store.required_capabilities = COG_STORE_CAP_CONTEXT_V3 | COG_STORE_CAP_TOOL_JOBS_V4;
  c.scopes = &scope;
  c.scope_count = 1U;
  c.tools.bindings = bindings;
  c.tools.binding_count = 2U;
  c.tools.user = workspace;
  c.session.max_tool_jobs = 2U;
  c.session.max_tool_job_output_bytes = 64U;
  c.background_job_timeout_ms = 15000U;
  cog_runtime_t *runtime = NULL;
  assert(cog_runtime_new(&runtime,&c) == COG_OK);
  assert(cog_runtime_begin(runtime) == COG_OK);
  unsigned submitted = 0U;
  bool receipt = false, unrelated = false, lookup = false, final = false, released = false;
  cog_runtime_step_t step = {0};
  for (unsigned i = 0U; i < 12000U && !step.done; i++) {
    cog_runtime_view_t view;
    cog_result_t rc = cog_runtime_view(runtime,&view);
    if (rc == COG_OK) {
      for (size_t j = 0U; j < view.item_count; j++) {
        const cog_runtime_view_item_t *item = &view.items[j];
        if (item->role == COG_MESSAGE_ROLE_TOOL) {
          receipt |= contains(item->text,item->text_bytes,"job-0000000000000001") &&
              contains(item->text,item->text_bytes,"accepted");
          lookup |= contains(item->text,item->text_bytes,cancel ? "\"cancelled\"" : "job-output");
        }
        if (item->role == COG_MESSAGE_ROLE_ASSISTANT) {
          unrelated |= contains(item->text,item->text_bytes,"unrelated answer");
          final |= contains(item->text,item->text_bytes,"job checked");
        }
      }
      assert(cog_runtime_ack(runtime,view.serial) == COG_OK);
    } else assert(rc == COG_ERROR_AGAIN);
    rc = cog_runtime_step(runtime,&step);
    assert(rc == COG_OK || rc == COG_ERROR_AGAIN);
    if (step.input_ready && !step.publication_pending && submitted == 0U) {
      assert(cog_runtime_submit(runtime,"launch",6U,false) == COG_OK);
      submitted++;
    } else if (step.input_ready && !step.publication_pending && submitted == 1U && receipt) {
      cog_tool_job_record_t job;
      assert(cog_runtime_job(runtime,1U,&job) == COG_OK && job.state == COG_TOOL_JOB_ACCEPTED);
      assert(cog_runtime_fork(runtime,0U) == COG_ERROR_AGAIN);
      cog_store_t *inspection = NULL;
      cog_session_t *loaded = NULL;
      cog_store_load_report_t load;
      assert(cog_store_new(&inspection,&c.store) == COG_OK);
      assert(cog_store_load_ex(inspection,c.session_id,COG_STORE_LOAD_REPLAY,&loaded,&load)
             == COG_ERROR_INDETERMINATE);
      assert(loaded == NULL);
      assert(cog_store_load_ex(inspection,c.session_id,COG_STORE_LOAD_INSPECT,&loaded,&load) == COG_OK);
      assert(load.indeterminate_jobs == 1U && load.indeterminate_tools == 0U);
      cog_tool_job_record_t projected;
      assert(cog_session_tool_job(loaded,1U,&projected) == COG_OK);
      assert(projected.state == COG_TOOL_JOB_INDETERMINATE && projected.receipt_node_id == job.receipt_node_id);
      cog_session_free(loaded);
      cog_store_free(inspection);
      assert(cog_runtime_submit(runtime,"another prompt",14U,false) == COG_OK);
      submitted++;
    } else if (unrelated && !released) {
      cog_tool_job_record_t job;
      assert(cog_runtime_job(runtime,1U,&job) == COG_OK && job.state == COG_TOOL_JOB_ACCEPTED);
      if (cancel) {
        assert(cog_runtime_job_cancel(runtime,1U) == COG_OK);
        assert(cog_runtime_job_cancel(runtime,1U) == COG_OK);
      } else {
        int root = open(directory,O_RDONLY|O_DIRECTORY);
        assert(root >= 0);
        int gate = openat(root,"release",O_WRONLY|O_CREAT|O_EXCL,0600);
        assert(gate >= 0 && close(gate) == 0 && close(root) == 0);
      }
      released = true;
    } else if (released && submitted == 2U && step.input_ready && !step.publication_pending) {
      cog_tool_job_record_t job;
      assert(cog_runtime_job(runtime,1U,&job) == COG_OK);
      if (job.state == COG_TOOL_JOB_FINISHED) {
        assert(job.outcome != NULL && job.outcome->status == (cancel ? COG_ERROR_ABORTED : COG_OK));
        if (!cancel) assert(strcmp(job.outcome->output,"job-output") == 0);
        assert(cog_runtime_submit(runtime,"check",5U,false) == COG_OK);
        submitted++;
      }
    }
    if (final && lookup) assert(cog_runtime_shutdown(runtime) == COG_OK);
    if (!step.done) (void)nanosleep(&(struct timespec){0,1000000L},NULL);
  }
  assert(step.done && submitted == 3U && receipt && unrelated && lookup && final);
  assert(cog_runtime_close(&runtime,COG_OK) == COG_OK);
  assert(cog_workspace_free(workspace) == COG_OK);

  cog_store_t *store = NULL;
  cog_session_t *session = NULL;
  cog_store_load_report_t report;
  cog_session_snapshot_t saved;
  assert(cog_store_new(&store,&c.store) == COG_OK);
  assert(cog_store_load_ex(store,c.session_id,COG_STORE_LOAD_REPLAY,&session,&report) == COG_OK);
  assert(report.source_version == COG_STORE_FORMAT_V4 && report.indeterminate_jobs == 0U);
  assert(cog_session_snapshot_view(session,&saved) == COG_OK && saved.tool_job_count == 1U);
  const cog_tool_job_record_t *job = &saved.tool_jobs[0];
  assert(job->state == COG_TOOL_JOB_FINISHED && job->outcome != NULL);
  assert(job->outcome->status == (cancel ? COG_ERROR_ABORTED : COG_OK));
  if (!cancel) assert(strcmp(job->outcome->output,"job-output") == 0);
  size_t launches = 0U, lookups = 0U;
  for (size_t i = 0U; i < saved.node_count; i++) {
    const cog_message_t *msg = saved.nodes[i].message;
    if (msg == NULL || msg->role != COG_MESSAGE_ROLE_TOOL) continue;
    if (strcmp(msg->tool_call_id,"lookup") == 0) lookups++;
    if (strcmp(msg->tool_call_id,"launch") == 0) {
      launches++;
      assert(job->receipt_node_id == saved.nodes[i].node_id);
    }
  }
  assert(launches == 1U && lookups == 1U);
  cog_session_free(session);
  cog_store_free(store);
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1],"background-job") == 0) {
    background_job_round(false);
    background_job_round(true);
    return 0;
  }
  stream_without_terminal();
  close_before_begin();
  pinned_resume();
  coordinated_close();
  local_delegation(false,false);
  local_delegation(true,false);
  local_delegation(true,true);
  return 0;
}
