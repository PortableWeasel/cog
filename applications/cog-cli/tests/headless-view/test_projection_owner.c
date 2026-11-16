/** @file test_projection_owner.c @brief Actual replay/codec transaction oracles. */
#include "view_internal.h"
#include "../headless-codecs/check.h"

static bool fail_append;
cog_result_t __real_cog_ipc_replay_append(cog_ipc_replay_t *, uint16_t,
    const void *, size_t, uint64_t *);
cog_result_t __wrap_cog_ipc_replay_append(cog_ipc_replay_t *, uint16_t,
    const void *, size_t, uint64_t *);
cog_result_t __wrap_cog_ipc_replay_append(cog_ipc_replay_t *r, uint16_t type,
    const void *bytes, size_t length, uint64_t *sequence)
{
    return fail_append ? COG_ERROR_IO :
        __real_cog_ipc_replay_append(r, type, bytes, length, sequence);
}
int main(int argc, char **argv)
{
    (void)test_negative_control(argc, argv);
    cog_cli_projection_config_t cfg = {"root", 7U, 2U, 4096U, 2048U, 512U, 1U, 16U, 32U};
    cog_cli_projection_t *p = NULL;
    CHECK(cog_cli_projection_new(NULL, &cfg) == COG_ERROR_NULL_PTR);
    CHECK(cog_cli_projection_new(&p, NULL) == COG_ERROR_NULL_PTR && p == NULL);
    cog_cli_projection_config_t bad = cfg;
    bad.projection_bytes = 1U;
    CHECK(cog_cli_projection_new(&p, &bad) == COG_ERROR_LIMIT && p == NULL);
    bad = cfg; bad.max_status_bytes = SIZE_MAX;
    CHECK(cog_cli_projection_new(&p, &bad) == COG_ERROR_LIMIT && p == NULL);
    bad = cfg; bad.records = SIZE_MAX;
    CHECK(cog_cli_projection_new(&p, &bad) == COG_ERROR_LIMIT && p == NULL);
    bad = cfg; bad.max_display_bytes = 1U;
    CHECK(cog_cli_projection_new(&p, &bad) == COG_ERROR_LIMIT && p == NULL);
    bad = cfg; bad.session_id = "bad/root";
    CHECK(cog_cli_projection_new(&p, &bad) == COG_ERROR_INVALID_ARG && p == NULL);
    OK(cog_cli_projection_new(&p, &cfg));
    cog_cli_projection_view_t snapshot;
    CHECK(cog_cli_projection_snapshot(p, &snapshot) == COG_ERROR_AGAIN);
    char text[] = "original";
    cog_cli_display_item_t item = {1U, COG_MESSAGE_ROLE_USER, COG_MESSAGE_COMPLETE, text, 8U};
    cog_cli_projection_view_t source = {{"root", 99U, 88U, &item, 1U}, 1U, 1U, NULL, 0U, NULL, 0U};
    uint64_t sequence = 999U;
    OK(cog_cli_projection_commit(p, &source, &sequence)); CHECK(sequence == 1U);
    OK(cog_cli_projection_snapshot(p, &snapshot));
    CHECK(snapshot.display.generation == 7U && snapshot.display.through_sequence == 1U);
    text[0] = 'X'; CHECK(snapshot.display.items[0].text[0] == 'o');
    fail_append = true;
    CHECK(cog_cli_projection_commit(p, &source, &sequence) == COG_ERROR_IO);
    CHECK(sequence == 1U && p->sequence == 1U);
    OK(cog_cli_projection_snapshot(p, &snapshot)); CHECK(snapshot.display.items[0].text[0] == 'o');
    unsigned char storage[4096]; size_t required = 0U;
    cog_ipc_replay_record_t record;
    OK(cog_cli_projection_copy_after(p, 7U, 0U, &record, storage, sizeof(storage), &required));
    CHECK(record.kind == COG_IPC_REPLAY_DATA && record.sequence == 1U);
    CHECK(memcmp(record.payload, "CGV1", 4U) == 0);
    CHECK(cog_cli_projection_copy_after(p, 7U, 1U, &record, storage, sizeof(storage), &required) == COG_ERROR_AGAIN);
    fail_append = false;
    source.active_configuration = 0U;
    CHECK(cog_cli_projection_commit(p, &source, &sequence) == COG_ERROR_INVALID_ARG);
    CHECK(sequence == 1U && p->sequence == 1U);
    source.active_configuration = 1U;
    OK(cog_cli_projection_commit(p, &source, &sequence)); CHECK(sequence == 2U);
    OK(cog_cli_projection_commit(p, &source, &sequence)); CHECK(sequence == 3U);
    OK(cog_cli_projection_copy_after(p, 7U, 0U, &record, storage, sizeof(storage), &required));
    CHECK(record.kind == COG_IPC_REPLAY_GAP && record.first_sequence == 2U && record.last_sequence == 3U);
    OK(cog_cli_projection_copy_after(p, 8U, 0U, &record, storage, sizeof(storage), &required));
    CHECK(record.kind == COG_IPC_REPLAY_GAP && record.generation == 7U);
    OK(cog_cli_projection_snapshot(p, &snapshot));
    OK(cog_cli_projection_commit(p, &snapshot, &sequence)); CHECK(sequence == 4U);
    p->sequence = UINT64_MAX;
    CHECK(cog_cli_projection_commit(p, &source, &sequence) == COG_ERROR_LIMIT && sequence == 4U);
    cog_cli_projection_free(p); cog_cli_projection_free(NULL);
    return 0;
}
