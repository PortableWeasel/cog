#include "composition_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static cog_handler_result_t observe(cog_core_t *c, const cog_event_t *e)
{ (void)c; (void)e; return (cog_handler_result_t){COG_TRANSITION_SELF, COG_OK}; }
static const cog_state_t transitions[] = {0U, COG_TRANSITION_END};
static const cog_dispatch_entry_t dispatch[] = {
 {0U, COG_EVENT_SUBAGENT_SPAWN, observe, transitions},
 {0U, COG_EVENT_SUBAGENT_CANCEL, observe, transitions}};
static void prefix(size_t records)
{
 cog_session_t *session = NULL;
 cog_session_config_t sc = {.id="control",.max_nodes=4U,.max_payload_bytes=4096U,.max_tool_calls_per_message=1U};
 assert(cog_session_new(&session,&sc)==COG_OK);
 cog_core_t *core=NULL;
 cog_core_config_t cc={.session=session,.dispatch_table=dispatch,.dispatch_table_size=2U,
  .max_agents=records,.max_agent_message_bytes=1024U,.event_queue_capacity=8U,
  .pending_session_capacity=1U,.pending_messages_per_session=1U,.pending_message_bytes=16U};
 assert(cog_core_new(&core,&cc)==COG_OK);
 cli_executor_slot_t slots[4]={0};
 unsigned char tasks[]="task one\0task two\0";
 cli_group_record_t g={.phase=CLI_GROUP_GRANTED,.id=1U,.reservation=7U,.deadline_ms=100U,
  .credit=2U,.submitted=2U,.result_payload_cap=256U,.tasks={.data=tasks,.capacity=sizeof(tasks)},
  .task_offsets={0U,9U},.task_lengths={8U,8U},.members={{.slot=2U},{.slot=3U}}};
 strcpy(g.parent_session,"root"); strcpy(g.call_id,"call");
 cog_cli_host_runtime_t r={.control_core=core,.executors=slots,.groups=&g,.group_capacity=1U,
  .request_credit_held=2U,.next_nonce=1U,.now_ms=1U,.bounds={.status_bytes=128U,.executors=4U}};
 cog_store_checkpoint_t cp={3U};
 assert(cli_runtime_start(&r,7U,cp)==COG_ERROR_AGAIN);
 assert(g.admitted==1U && g.members[0].present && !g.gate_open);
 assert(r.request_credit_held==0U && r.request_credit_used==2U);
 assert(cli_runtime_start(&r,7U,(cog_store_checkpoint_t){4U})==COG_ERROR_PROTOCOL);
 cog_result_t rc=cli_runtime_start(&r,7U,cp);
 if(records==1U) {
  assert(rc<COG_OK && rc!=COG_ERROR_AGAIN);
  assert(g.phase==CLI_GROUP_ROLLBACK && !g.gate_open);
  assert(!g.members[1].present && g.members[1].agent==0U);
  assert(slots[2].cancel_latched && slots[3].cancel_latched);
  assert(r.request_credit_used==2U);
 } else {
  assert(rc==COG_OK && g.gate_open && g.admitted==2U);
  assert(slots[2].member_count==2U && slots[3].member_count==2U);
  cog_agent_id_t first=slots[2].agent,second=slots[3].agent;
  assert(first!=0U && second>first);
  assert(cli_runtime_start(&r,7U,cp)==COG_OK);
  assert(slots[2].agent==first && slots[3].agent==second);
  assert(cli_runtime_release(&r,7U)==COG_OK && !g.gate_open);
  assert(r.request_credit_used==2U);
 }
 /* Real core consumes queued intent leases; no execution or session borrow. */
 cog_core_free(core); cog_session_free(session);
}
int main(void){prefix(1U);prefix(2U);puts("real registry group prefix: PASS");return 0;}
