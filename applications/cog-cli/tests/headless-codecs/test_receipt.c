/** @file test_receipt.c @brief Literal, predicates, malformed and transactional receipt tests. */
#include "control_receipt.h"
#include <stdbool.h>
#include "check.h"

static const unsigned char golden[55] = {
    1,3, 0,0,0,0,0,0,0,1, 0,0,0,0,0,0,0,2,
    0,0,0,0,0,0,0,3, 0,0,0,0,0,0,0,4,
    0,0,0,0,0,0,0,5, 0,0,0,0,0,0,0,0, 0, 0,0,0,0
};
static void malformed(const unsigned char *bytes, size_t length, cog_result_t error)
{
    cog_cli_control_receipt_t out;
    test_fill(&out, sizeof(out));
    CHECK(cog_cli_control_receipt_decode(bytes, length, &out) == error);
    test_unchanged(&out, sizeof(out));
}
int main(int argc, char **argv)
{
    (void)test_negative_control(argc, argv);
    cog_cli_control_receipt_t r = {.parent_runtime_incarnation=1, .agent_id=2, .execution_epoch=3,
        .owner_peer=4, .wire_id=5, .disposition=COG_CLI_CONTROL_RECEIPT_CANCEL_CONSUMED};
    unsigned char bytes[64]; size_t required = 999U;
    test_fill(bytes, sizeof(bytes));
    CHECK(cog_cli_control_receipt_encode(&r, bytes, 54U, &required) == COG_ERROR_OVERFLOW);
    CHECK(required == 55U); test_unchanged(bytes, sizeof(bytes));
    OK(cog_cli_control_receipt_encode(&r, bytes, sizeof(bytes), &required));
    CHECK(memcmp(bytes, golden, sizeof(golden)) == 0); test_unchanged(bytes + 55U, 9U);
    cog_cli_control_receipt_t out;
    OK(cog_cli_control_receipt_decode(golden, sizeof(golden), &out));
    CHECK(out.parent_runtime_incarnation == 1U && out.disposition == COG_CLI_CONTROL_RECEIPT_CANCEL_CONSUMED);
    for (size_t n = 0U; n < 55U; ++n) malformed(golden, n, COG_ERROR_PROTOCOL);
    malformed(bytes, 56U, COG_ERROR_PROTOCOL);
    for (size_t i = 0U; i < 5U; ++i) {
        memcpy(bytes, golden, 55U); memset(bytes + 2U + i * 8U, 0, 8U);
        malformed(bytes, 55U, COG_ERROR_PROTOCOL);
    }
    memcpy(bytes, golden, 55U); bytes[0]=0; malformed(bytes,55U,COG_ERROR_PROTOCOL);
    bytes[0]=2; bytes[1]=0; malformed(bytes,55U,COG_ERROR_UNSUPPORTED);
    bytes[0]=1; malformed(bytes,55U,COG_ERROR_PROTOCOL);
    bytes[1]=5; malformed(bytes,55U,COG_ERROR_UNSUPPORTED);
    memcpy(bytes,golden,55U); bytes[50]=1; malformed(bytes,55U,COG_ERROR_PROTOCOL);
    memcpy(bytes,golden,55U); bytes[54]=1; malformed(bytes,55U,COG_ERROR_PROTOCOL);
    r.wire_id=UINT64_MAX; required=999U; test_fill(bytes,sizeof(bytes));
    CHECK(cog_cli_control_receipt_encode(&r,NULL,0,&required)==COG_ERROR_INVALID_ARG); CHECK(required==999U);
    r.wire_id=5; r.parent_accepted_sequence=UINT64_MAX;
    CHECK(cog_cli_control_receipt_encode(&r,NULL,0,&required)==COG_ERROR_INVALID_ARG);
    r.parent_accepted_sequence=1; r.disposition=COG_CLI_CONTROL_RECEIPT_ACCEPTED;
    for (int status=0; status<=5; ++status) for (int result=-19; result<=1; ++result) {
        r.accepted_terminal_status=(cog_agent_status_t)status; r.accepted_terminal_outcome=(cog_result_t)result;
        bool valid = ((status==0 || status==2) && result==0) || (status>=3 && result<0 && result>=-18);
        cog_result_t rc=cog_cli_control_receipt_encode(&r,bytes,sizeof(bytes),&required);
        CHECK(rc==(valid ? COG_OK : COG_ERROR_INVALID_ARG));
        if (valid) { OK(cog_cli_control_receipt_decode(bytes,55U,&out)); CHECK(out.accepted_terminal_outcome==(cog_result_t)result); }
    }
    r.accepted_terminal_status=COG_AGENT_STATUS_FAILED; r.accepted_terminal_outcome=COG_ERROR_DENIED;
    r.disposition=COG_CLI_CONTROL_RECEIPT_TERMINAL_FIXED;
    OK(cog_cli_control_receipt_encode(&r,bytes,sizeof(bytes),&required));
    CHECK(bytes[51]==255U && bytes[54]==240U); OK(cog_cli_control_receipt_decode(bytes,55U,&out));
    r.disposition=COG_CLI_CONTROL_RECEIPT_RETRY; r.parent_accepted_sequence=UINT64_MAX;
    OK(cog_cli_control_receipt_encode(&r,bytes,sizeof(bytes),&required));
    r.accepted_terminal_status=COG_AGENT_STATUS_PENDING; r.accepted_terminal_outcome=COG_OK;
    r.parent_accepted_sequence=0;
    OK(cog_cli_control_receipt_encode(&r,bytes,sizeof(bytes),&required));
    CHECK(cog_cli_control_receipt_encode(&r,&r,sizeof(r),&required)==COG_ERROR_INVALID_ARG);
    CHECK(cog_cli_control_receipt_decode(bytes,55U,(cog_cli_control_receipt_t *)(void *)bytes)==COG_ERROR_PROTOCOL);
    CHECK(cog_cli_control_receipt_encode(NULL,NULL,0,&required)==COG_ERROR_NULL_PTR);
    CHECK(cog_cli_control_receipt_decode(NULL,55U,&out)==COG_ERROR_NULL_PTR);
    return 0;
}
