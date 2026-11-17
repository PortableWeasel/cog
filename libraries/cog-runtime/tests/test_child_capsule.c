#include "../src/control.h"
#include <assert.h>
#include <string.h>
int main(void)
{
    cog_cli_instruction_identity_t id={.catalog_generation=3U,.snapshot_id=9U};
    memset(id.sha256,0xAB,32U);
    unsigned char body[]={0,1,2,3},wire[60],sentinel[60];size_t required=0;
    memset(wire,0xCD,sizeof(wire));memcpy(sentinel,wire,sizeof(wire));
    assert(cli_child_start_wrap(&id,body,sizeof(body),wire,1,&required)==COG_ERROR_OVERFLOW);
    assert(required==60U && memcmp(wire,sentinel,sizeof(wire))==0);
    assert(cli_child_start_wrap(&id,body,sizeof(body),wire,sizeof(wire),&required)==COG_OK);
    assert(memcmp(wire,"CGS2",4)==0 && wire[11]==3U && wire[19]==9U && wire[55]==4U);
    cog_cli_instruction_identity_t got={0};const void *legacy=NULL;size_t bytes=0;
    assert(cli_child_start_unwrap(wire,sizeof(wire),&got,&legacy,&bytes)==COG_OK);
    assert(got.snapshot_id==9U && got.catalog_generation==3U && memcmp(got.sha256,id.sha256,32)==0);
    assert(legacy==wire+56 && bytes==4U && memcmp(legacy,body,4)==0);
    for(size_t i=0;i<sizeof(wire);++i) {
        legacy=body;bytes=123U;got.snapshot_id=100U;
        assert(cli_child_start_unwrap(wire,i,&got,&legacy,&bytes)!=COG_OK);
        assert(legacy==body && bytes==123U && got.snapshot_id==100U);
    }
    wire[3]='3';assert(cli_child_start_unwrap(wire,sizeof(wire),&got,&legacy,&bytes)==COG_ERROR_UNSUPPORTED);
    wire[3]='2';wire[11]=0U;
    assert(cli_child_start_unwrap(wire,sizeof(wire),&got,&legacy,&bytes)==COG_ERROR_PROTOCOL);
    id=(cog_cli_instruction_identity_t){0};
    assert(cli_child_start_wrap(&id,body,sizeof(body),wire,sizeof(wire),&required)==COG_OK);
    assert(cli_child_start_unwrap(wire,sizeof(wire),&got,&legacy,&bytes)==COG_OK);
    wire[20]=1U;assert(cli_child_start_unwrap(wire,sizeof(wire),&got,&legacy,&bytes)==COG_ERROR_PROTOCOL);
    return 0;
}
