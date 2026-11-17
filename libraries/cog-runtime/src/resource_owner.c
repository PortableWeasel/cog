#include "execution_internal.h"
#include "runtime_internal.h"
#include "resource_owner.h"
#include <stdlib.h>
#include <string.h>
cog_result_t cli_resource_owner_new(cli_resource_owner_t **out) {
    if(out==NULL)return COG_ERROR_NULL_PTR;
    *out=calloc(1U,sizeof(**out));
    return *out==NULL?COG_ERROR_NOMEM:COG_OK;
}
cog_result_t cli_resource_owner_retire(cli_resource_owner_t *owner) {
    if(owner==NULL)return COG_OK;
    cog_result_t rc=cli_runtime_resolver_free(&owner->job);
    if(owner->job!=NULL)return rc;
    cog_instruction_snapshot_free(owner->candidate);owner->candidate=NULL;
    cog_session_free(owner->proof);owner->proof=NULL;
    cog_session_free(owner->saved_proof);owner->saved_proof=NULL;
    owner->prepared=false;owner->reply_wire=0U;owner->candidate_id=0U;
    return rc;
}
cog_result_t cli_resource_owner_free(cli_resource_owner_t **out) {
    if(out==NULL)return COG_ERROR_NULL_PTR;
    if(*out==NULL)return COG_OK;
    cog_result_t rc=cli_resource_owner_retire(*out);
    if((*out)->job!=NULL)return rc;
    cog_instruction_snapshot_free((*out)->current);free(*out);*out=NULL;
    return rc;
}

cog_result_t cli_resource_owner_reset(cli_resource_owner_t *owner) {
    if(owner==NULL)return COG_OK;
    cog_result_t rc=cli_resource_owner_retire(owner);
    if(owner->job!=NULL)return rc;
    cog_instruction_snapshot_free(owner->current);
    memset(owner,0,sizeof(*owner));return rc;
}
