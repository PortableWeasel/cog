/** @file test_handoff_leaf.c
 * Actual codecs/cache boundaries and actual local staged/store/drain operations.
 * Includes the leaf to inspect its private no-effect operations. No API stubs.
 * This is not a replacement for the separate multi-process host join tests.
 */
#include "../src/handoff.c"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

typedef struct fixture {
    cog_cli_host_runtime_t runtime;
    cog_cli_handoff_t handoff;
    cli_executor_slot_t slots[2];
    cli_worker_record_t workers[2];
    cli_control_lane_t lanes[2][CLI_HOST_LANES];
    unsigned char pending[512], reply[512], storage[2][3][512];
} fixture_t;
static void fixture_init(fixture_t *f)
{
    memset(f, 0, sizeof(*f));
    f->runtime.executors = f->slots; f->runtime.workers = f->workers;
    f->runtime.bounds.workers = 2U; f->runtime.bounds.executors = 2U;
    f->runtime.runtime_incarnation = 91U; strcpy(f->runtime.primary_session, "handoff-root");
    f->handoff.runtime = &f->runtime; f->handoff.source_slot = 0U; f->handoff.target_slot = 1U;
    f->handoff.source_worker = (cog_worker_id_t){7U, 9U};
    f->handoff.target_worker = (cog_worker_id_t){8U, 10U};
    f->handoff.source_peer = 11U; f->handoff.target_peer = 22U;
    f->handoff.expected = (cog_store_owner_t){1U, 101U};
    f->handoff.next = (cog_store_owner_t){2U, 102U};
    f->handoff.identity = 100U; f->handoff.deadline_ms = 500U;
    f->handoff.pending = (cli_host_bytes_t){f->pending, sizeof(f->pending), 0U};
    f->handoff.reply = (cli_host_bytes_t){f->reply, sizeof(f->reply), 0U};
    for (size_t i = 0U; i < 2U; ++i) {
        f->slots[i].worker = i == 0U ? f->handoff.source_worker : f->handoff.target_worker;
        f->slots[i].owner_peer = i == 0U ? 11U : 22U;
        f->slots[i].outbound = f->lanes[i];
        cli_control_lane_t *lane = &f->lanes[i][COG_CLI_LANE_HANDOFF - 1U];
        lane->pending = (cli_host_bytes_t){f->storage[i][0], 512U, 0U};
        lane->final_original = (cli_host_bytes_t){f->storage[i][1], 512U, 0U};
        lane->final_reply = (cli_host_bytes_t){f->storage[i][2], 512U, 0U};
        f->workers[i].used = true; f->workers[i].id = f->slots[i].worker;
        f->workers[i].native.peer_id = f->slots[i].owner_peer;
    }
}
static int reply(fixture_t *f, size_t index, cog_handoff_kind_t kind, uint64_t checkpoint,
    uint64_t reply_to, bool wrong_owner)
{
    unsigned char body[189]; size_t required;
    cog_handoff_message_t nested = {.kind = kind, .session_id = f->runtime.primary_session,
        .source_peer = 11U, .target_peer = 22U, .expected = f->handoff.expected,
        .next = f->handoff.next, .checkpoint = {checkpoint}};
    if (wrong_owner) ++nested.next.instance_id;
    CHECK(cog_orchestrator_handoff_encode(&nested, body, sizeof(body), &required) == COG_OK);
    cog_cli_control_message_t outer = {.kind = COG_CLI_CONTROL_HANDOFF, .lane = COG_CLI_LANE_HANDOFF,
        .runtime_incarnation = 91U, .sender_incarnation = f->slots[index].worker.incarnation,
        .wire_id = 50U, .reply_to = reply_to, .worker = f->slots[index].worker,
        .owner_peer = f->slots[index].owner_peer, .session_id = f->runtime.primary_session,
        .payload = body, .payload_len = required};
    CHECK(cog_cli_control_encode(&outer, f->reply, sizeof(f->reply), &f->handoff.reply.length) == COG_OK);
    return 0;
}
static int protocol(void)
{
    fixture_t f; fixture_init(&f); cog_cli_handoff_t *h = &f.handoff;
    CHECK(handoff_queue(h, 0U, COG_HANDOFF_QUIESCE, 0U) == COG_OK);
    unsigned char original[512]; size_t bytes = h->pending.length;
    memcpy(original, h->pending.data, bytes);
    CHECK(handoff_queue(h, 0U, COG_HANDOFF_QUIESCE, 0U) == COG_OK);
    CHECK(bytes == h->pending.length && memcmp(original, h->pending.data, bytes) == 0);
    CHECK(reply(&f, 0U, COG_HANDOFF_CHECKPOINTED, 73U, 99U, false) == 0);
    CHECK(handoff_receive(h, 0U, COG_HANDOFF_CHECKPOINTED) == COG_ERROR_PROTOCOL);
    CHECK(h->checkpoint.generation == 0U && h->pending.length == bytes);
    CHECK(reply(&f, 0U, COG_HANDOFF_CHECKPOINTED, 73U, 100U, true) == 0);
    CHECK(handoff_receive(h, 0U, COG_HANDOFF_CHECKPOINTED) == COG_ERROR_PROTOCOL);
    CHECK(h->checkpoint.generation == 0U && !h->source_released && !h->cas_committed);
    CHECK(reply(&f, 0U, COG_HANDOFF_CHECKPOINTED, 73U, 100U, false) == 0);
    CHECK(handoff_receive(h, 0U, COG_HANDOFF_CHECKPOINTED) == COG_OK);
    cli_control_lane_t *lane = &f.lanes[0][COG_CLI_LANE_HANDOFF - 1U];
    CHECK(h->checkpoint.generation == 73U && h->pending.length == 0U && h->reply.length == 0U);
    CHECK(lane->final_valid && !lane->pending_valid && lane->final_answered_wire == 100U);
    CHECK(lane->final_original.length == bytes && memcmp(original, lane->final_original.data, bytes) == 0);
    CHECK(handoff_queue(h, 0U, COG_HANDOFF_PREPARED, 2U) == COG_OK);
    CHECK(lane->retired_through == 100U && !lane->final_valid);
    CHECK(reply(&f, 1U, COG_HANDOFF_PREPARED, 73U, 102U, false) == 0);
    CHECK(handoff_receive(h, 0U, COG_HANDOFF_PREPARED) == COG_ERROR_PROTOCOL);
    CHECK(!h->source_released && !h->cas_committed);
    CHECK(reply(&f, 0U, COG_HANDOFF_PREPARED, 74U, 102U, false) == 0);
    CHECK(handoff_receive(h, 0U, COG_HANDOFF_PREPARED) == COG_ERROR_PROTOCOL);
    CHECK(reply(&f, 0U, COG_HANDOFF_PREPARED, 73U, 102U, false) == 0);
    CHECK(handoff_receive(h, 0U, COG_HANDOFF_PREPARED) == COG_OK);
    CHECK(handoff_queue(h, 0U, COG_HANDOFF_ABORT, 12U) == COG_OK);
    CHECK(reply(&f, 0U, COG_HANDOFF_ABORT, 73U, 112U, false) == 0);
    CHECK(handoff_receive(h, 0U, COG_HANDOFF_ABORT) == COG_OK);
    CHECK(lane->final_valid && lane->final_answered_wire == 112U);
    /* Physical slot reuse/PROFILE clearing cannot touch HANDOFF evidence. */
    handoff_secret_clear(&f.slots[0].profile); f.slots[0].phase = CLI_SLOT_FREE;
    CHECK(lane->final_valid && lane->final_answered_wire == 112U);
    CHECK(handoff_lane_ready(&f.slots[0], &f.runtime, (cog_worker_id_t){99U, 2U}, 11U) == COG_ERROR_AGAIN);
    CHECK(handoff_queue(h, 0U, COG_HANDOFF_PREPARED, 14U) == COG_OK);
    f.workers[0].native.reaped = true;
    CHECK(handoff_native_terminal(h, 0U));
    CHECK(h->abort_requested && h->primary == COG_ERROR_IO && f.runtime.admission_frozen);
    CHECK(f.slots[0].local_done && f.slots[0].phase == CLI_SLOT_RETAINED);
    CHECK(h->pending.length == 0U && lane->pending_valid && lane->pending.length != 0U);
    return 0;
}
static int local_operations(const char *base, const char *socket)
{
    cog_cli_options_t options = {.backend = COG_CLI_BACKEND_MOCK, .model_id = "current/model",
        .config_path = "/dev/null", .store_mode = COG_CLI_STORE_DURABLE, .store_dir = base,
        .session_id = "local-stages", .session_action = COG_CLI_SESSION_NEW,
        .explicit_options = COG_CLI_OPT_BACKEND | COG_CLI_OPT_MODEL | COG_CLI_OPT_STORE_MODE | COG_CLI_OPT_STORE_DIR};
    cog_cli_configuration_t *configuration = NULL;
    CHECK(cog_cli_configuration_new(&configuration, &options, 1U) == COG_OK);
    cog_cli_execution_admission_t admission = {.kind = COG_CLI_EXECUTION_ROOT,
        .configuration = configuration, .configuration_generation = 1U};
    cog_cli_app_t *source = NULL;
    CHECK(cog_cli_execution_new(&source, &admission) == COG_OK);
    CHECK(cog_cli_execution_begin(source) == COG_OK);
    uint64_t serial; cog_cli_display_snapshot_t view;
    CHECK(cog_cli_execution_copy_view(source, &serial, &view) == COG_OK);
    CHECK(cog_cli_execution_commit_view(source, serial, 91U, 1U) == COG_OK);
    cog_core_step_info_t info;
    for (size_t i = 0U; i < 8U; ++i) CHECK(cog_cli_execution_step(source, &info) == COG_OK);
    CHECK(handoff_local_quiet(source) == COG_OK);
    fixture_t f; fixture_init(&f);
    f.runtime.current_mirror = NULL;
    CHECK(cog_cli_configuration_clone_new(&f.runtime.current_mirror, source->configuration, NULL) == COG_OK);
    cog_store_t *catalog = NULL;
    CHECK(cog_store_new(&catalog, &(cog_store_config_t){.base_dir = base, .require_owner = true}) == COG_OK);
    cog_orchestrator_t *orch = NULL;
    CHECK(cog_orchestrator_new(&orch, &(cog_orchestrator_config_t){.socket_path = socket,
        .max_aux_cores = 1U, .store = catalog, .message_bytes = 4096U, .max_frame_bytes = 512U}) == COG_OK);
    cog_cli_handoff_t *owner = NULL;
    CHECK(cog_cli_handoff_new(&owner, &f.runtime, orch, catalog, 0U) == COG_ERROR_INVALID_ARG && owner == NULL);
    CHECK(cog_cli_handoff_new(&owner, &f.runtime, orch, catalog, 100U) == COG_OK);
    CHECK(cog_cli_handoff_request(NULL, (cog_worker_id_t){0}) == COG_ERROR_NULL_PTR);
    f.runtime.begun = true; f.runtime.next_nonce = 300U;
    f.runtime.active_configuration = 1U; f.runtime.published_configuration = 1U;
    strcpy(f.runtime.primary_session, "local-stages");
    cog_store_checkpoint_t initial;
    CHECK(cog_store_owner_read(catalog, "local-stages", &f.runtime.primary_owner, &initial) == COG_OK);
    f.slots[0].worker = (cog_worker_id_t){0}; f.slots[0].owner_peer = 0U;
    f.slots[0].owner = f.runtime.primary_owner; f.slots[0].app = source;
    f.workers[1].profile_ready = true; f.workers[1].native.health = COG_HEALTH_HEALTHY;
    CHECK(cog_cli_handoff_request(owner, (cog_worker_id_t){0}) == COG_ERROR_INVALID_STATE);
    f.lanes[0][COG_CLI_LANE_VIEW - 1U].pending_valid = true;
    CHECK(cog_cli_handoff_request(owner, f.workers[1].id) == COG_ERROR_AGAIN);
    CHECK(owner->phase == CLI_HANDOFF_IDLE && f.runtime.next_nonce == 300U &&
        source->headless->authority == CLI_AUTH_NORMAL && f.slots[1].phase == CLI_SLOT_FREE);
    f.lanes[0][COG_CLI_LANE_VIEW - 1U].pending_valid = false;
    f.runtime.next_nonce = UINT64_MAX - 1U;
    CHECK(cog_cli_handoff_request(owner, f.workers[1].id) == COG_ERROR_LIMIT);
    CHECK(owner->phase == CLI_HANDOFF_IDLE && source->headless->authority == CLI_AUTH_NORMAL);
    CHECK(cog_cli_handoff_close(&owner, COG_OK) == COG_OK && owner == NULL);
    cog_cli_handoff_t *h = &f.handoff; h->catalog = catalog; h->orchestrator = orch;
    strcpy(f.runtime.primary_session, "local-stages");
    f.slots[0].app = source; f.slots[0].owner_peer = 0U; f.slots[0].started = true;
    f.slots[1].owner_peer = 0U; f.slots[1].configuration_generation = 1U;
    h->target_peer = 0U;
    CHECK(cog_store_owner_read(catalog, "local-stages", &h->expected, &h->checkpoint) == COG_OK);
    h->next = (cog_store_owner_t){h->expected.epoch + 1U, h->expected.instance_id ^ UINT64_C(0x100)};
    CHECK(handoff_local_reserve(h, &f.slots[1]) == COG_OK);
    CHECK(cog_cli_execution_quiesce(source, h->expected, &h->checkpoint) == COG_OK);
    cog_store_checkpoint_t same = {0};
    CHECK(cog_cli_execution_quiesce(source, h->expected, &same) == COG_OK && same.generation == h->checkpoint.generation);
    CHECK(handoff_prepare(h) == COG_OK);
    cog_cli_app_t *target = f.slots[1].app;
    CHECK(target != NULL && target->guard == NULL && target->headless->authority == CLI_AUTH_STAGED);
    CHECK(cog_store_owner_compare_exchange(catalog, "local-stages", h->expected, h->next, h->checkpoint) == COG_ERROR_AGAIN);
    CHECK(cog_cli_execution_stage_guard(target, h->next, h->checkpoint) == COG_ERROR_INVALID_STATE);
    CHECK(cog_cli_execution_release_source(source, h->checkpoint) == COG_OK);
    CHECK(cog_cli_execution_release_source(source, h->checkpoint) == COG_OK);
    CHECK(cog_store_owner_compare_exchange(catalog, "local-stages", h->expected, h->next, h->checkpoint) == COG_OK);
    CHECK(cog_cli_execution_stage_guard(target, h->next, h->checkpoint) == COG_OK);
    CHECK(target->headless->authority == CLI_AUTH_TARGET_GUARDED);
    CHECK(cog_orchestrator_publish_owner(orch, "local-stages", 0U, h->next, h->checkpoint) == COG_OK);
    CHECK(cog_cli_execution_stage_activate(target, h->next, h->checkpoint) == COG_OK);
    CHECK(cog_cli_execution_stage_activate(target, h->next, h->checkpoint) == COG_OK);
    CHECK(cog_cli_execution_begin(target) == COG_OK);
    cog_mock_state_t mock;
    CHECK(cog_mock_provider_state(target->provider, &mock) == COG_OK && mock.send_count == 0U);
    for (size_t slot = 0U; slot < 2U; ++slot) {
        cog_result_t rc = COG_ERROR_AGAIN;
        for (size_t i = 0U; i < 64U && rc == COG_ERROR_AGAIN; ++i) rc = handoff_local_dispose(&f.slots[slot]);
        CHECK(rc == COG_OK && f.slots[slot].app == NULL && f.slots[slot].local_done);
    }
    cog_cli_configuration_free(f.runtime.current_mirror);
    cog_orchestrator_free(orch); cog_store_free(catalog);
    return 0;
}
int main(int argc, char **argv)
{
    CHECK(argc == 3);
    CHECK(protocol() == 0);
    CHECK(local_operations(argv[1], argv[2]) == 0);
    puts("handoff leaf: actual codec/cache and store/staged/drain checks PASS");
    return 0;
}
