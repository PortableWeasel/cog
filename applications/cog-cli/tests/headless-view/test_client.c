/** @file test_client.c @brief Native peer and real codecs, no runtime substitute. */
#define _POSIX_C_SOURCE 200809L
#include "view_internal.h"
#include "codec_internal.h"
#include "../headless-codecs/check.h"
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

typedef struct fixture {
    cog_cli_host_client_t *client;
    cog_ipc_endpoint_t *listener, *peer;
    FILE *output;
    int input[2];
    unsigned char receive[8192], storage[8192];
    uint64_t server_wire, runtime;
} fixture_t;
static uint64_t now(void)
{
    struct timespec ts; CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}
static void pump(fixture_t *f)
{
    cog_cli_host_runtime_step_t info;
    cog_result_t stepped = cog_cli_host_client_step(f->client, &info);
    if (stepped != COG_OK) (void)fprintf(stderr, "step=%d phase=%d hello=%d\n", (int)stepped, (int)f->client->phase, (int)f->client->hello.phase);
    OK(stepped);
    if (f->peer == NULL) {
        cog_result_t rc = cog_ipc_endpoint_accept(f->listener, &f->peer, NULL);
        CHECK(rc == COG_OK || rc == COG_ERROR_AGAIN);
    }
    if (f->peer != NULL) {
        uint64_t next;
        cog_result_t rc = cog_ipc_endpoint_service(f->peer, now(), &next);
        CHECK(rc == COG_OK || rc == COG_ERROR_AGAIN);
    }
}
static cog_cli_control_message_t next_frame(fixture_t *f)
{
    for (size_t i = 0U; i < 1000U; ++i) {
        pump(f);
        if (f->peer == NULL) continue;
        cog_ipc_message_t bytes = {f->receive, 0U, sizeof(f->receive)};
        cog_result_t rc = cog_ipc_endpoint_recv(f->peer, &bytes);
        if (rc == COG_ERROR_AGAIN) continue;
        OK(rc);
        cog_cli_control_message_t m; size_t required;
        OK(cog_cli_control_decode(bytes.data, bytes.len, &m, f->storage, sizeof(f->storage), &required));
        return m;
    }
    CHECK(false);
    cog_cli_control_message_t empty = {0}; return empty;
}
static void send_frame(fixture_t *f, cog_cli_control_kind_t kind, cog_cli_control_lane_t lane,
    uint64_t reply, const void *body, size_t length)
{
    cog_cli_control_message_t m = {0};
    m.kind = kind; m.lane = lane; m.runtime_incarnation = f->runtime; m.sender_incarnation = f->runtime;
    m.wire_id = ++f->server_wire; m.reply_to = reply; m.session_id = "root";
    m.payload = body; m.payload_len = length;
    unsigned char frame[8192]; size_t required;
    OK(cog_cli_control_encode(&m, frame, sizeof(frame), &required));
    cog_ipc_message_t bytes = {frame, required, sizeof(frame)};
    OK(cog_ipc_endpoint_send(f->peer, &bytes));
}
static void send_result(fixture_t *f, const cog_cli_control_message_t *request,
    cog_result_t result, uint64_t lease)
{
    unsigned char body[21]; cli_wire_writer_t w = {body, 0U, true};
    cli_wire_put(&w, (uint32_t)result, 4U); cli_wire_put(&w, 2U, 1U);
    cli_wire_put(&w, 0U, 8U); cli_wire_put(&w, lease, 8U);
    send_frame(f, COG_CLI_CONTROL_COMMAND_RESULT, request->lane, request->wire_id, body, sizeof(body));
}
static void setup(fixture_t *f, const char *path, bool watch)
{
    memset(f, 0, sizeof(*f)); f->runtime = 42U; CHECK(pipe(f->input) == 0);
    char output_path[512];
    CHECK(snprintf(output_path, sizeof(output_path), "%s.out", path) > 0);
    f->output = fopen(output_path, "w+"); CHECK(f->output != NULL);
    cog_ipc_endpoint_config_t ep = {0}; ep.max_frame_bytes = 70000U;
    OK(cog_ipc_endpoint_new_ex(&f->listener, &ep));
    OK(cog_ipc_endpoint_listen(f->listener, path));
    cog_cli_host_client_config_t config = {path, watch, false, f->output, f->output,
        f->input[0], -1, 4096U, 1000U};
    OK(cog_cli_host_client_new(&f->client, &config));
}
static void handshake(fixture_t *f)
{
    cog_cli_control_message_t h = next_frame(f);
    CHECK(h.kind == COG_CLI_CONTROL_UI_HELLO && h.runtime_incarnation == 0U && h.session_id[0] == '\0');
    send_result(f, &h, COG_OK, 77U);
    cog_cli_control_message_t ack = next_frame(f);
    CHECK(ack.kind == COG_CLI_CONTROL_COMMAND_ACK && ack.sender_incarnation == 77U);
    send_result(f, &ack, COG_OK, 0U);
}
static void view(fixture_t *f, uint64_t sequence, bool gap)
{
    cog_cli_display_item_t item = {1U, COG_MESSAGE_ROLE_USER, COG_MESSAGE_COMPLETE, "hi\033\n", 4U};
    cog_cli_projection_view_t v = {{"root", 9U, sequence, &item, 1U}, 1U, 1U, NULL, 0U, NULL, 0U};
    unsigned char payload[4096], replay[8192]; size_t length, bytes;
    OK(cog_cli_projection_encode(&v, payload, sizeof(payload), &length));
    cog_ipc_replay_record_t r = {gap ? COG_IPC_REPLAY_GAP : COG_IPC_REPLAY_DATA,
        "root", 9U, gap ? sequence - 1U : sequence, sequence, sequence,
        gap ? 0U : 1U, gap ? NULL : payload, gap ? 0U : length};
    OK(cog_ipc_replay_encode(&r, replay, sizeof(replay), &bytes));
    send_frame(f, COG_CLI_CONTROL_REPLAY, COG_CLI_LANE_REPLAY, 0U, replay, bytes);
    if (gap) send_frame(f, COG_CLI_CONTROL_RESYNC_VIEW, COG_CLI_LANE_REPLAY, 0U, payload, length);
}
static void cleanup(fixture_t *f)
{
    OK(cog_cli_host_client_close(&f->client, COG_OK)); CHECK(f->client == NULL);
    OK(cog_ipc_endpoint_free(f->peer)); OK(cog_ipc_endpoint_free(f->listener));
    CHECK(close(f->input[0]) == 0);
    if (f->input[1] >= 0) CHECK(close(f->input[1]) == 0);
    CHECK(fclose(f->output) == 0);
}

static void online(fixture_t *f)
{
    handshake(f); view(f, 1U, false);
    cog_cli_control_message_t m = next_frame(f);
    CHECK(m.kind == COG_CLI_CONTROL_REPLAY);
    for (size_t i = 0U; i < 20U; ++i) pump(f);
    CHECK(f->client->phase == CLI_CLIENT_ONLINE);
}
static void complete(fixture_t *f, cog_cli_control_message_t *m)
{
    send_result(f, m, COG_OK, 0U);
    cog_cli_control_message_t ack = next_frame(f);
    CHECK(ack.kind == COG_CLI_CONTROL_COMMAND_ACK && ack.lane == m->lane);
    send_result(f, &ack, COG_OK, 0U);
    for (size_t i = 0U; i < 20U; ++i) pump(f);
}
static void reconnect_case(const char *path)
{
    fixture_t f; setup(&f, path, false); online(&f);
    CHECK(write(f.input[1], "hello\n", 6U) == 6);
    cog_cli_control_message_t original = next_frame(&f);
    CHECK(original.kind == COG_CLI_CONTROL_UI_COMMAND);
    unsigned char saved[8192]; size_t saved_bytes;
    OK(cog_cli_control_encode(&original, saved, sizeof(saved), &saved_bytes));
    uint64_t original_wire = original.wire_id;
    OK(cog_ipc_endpoint_free(f.peer)); f.peer = NULL;
    cog_cli_control_message_t h = next_frame(&f);
    CHECK(h.kind == COG_CLI_CONTROL_UI_HELLO && h.wire_id > original_wire &&
        h.sender_incarnation == 77U && h.runtime_incarnation == 42U && strcmp(h.session_id, "root") == 0);
    CHECK(f.client->requests[0].phase == CLI_CLIENT_REQUEST_PENDING);
    send_result(&f, &h, COG_OK, 77U);
    cog_cli_control_message_t ha = next_frame(&f); CHECK(ha.kind == COG_CLI_CONTROL_COMMAND_ACK);
    send_result(&f, &ha, COG_OK, 0U);
    cog_cli_control_message_t retry = next_frame(&f);
    CHECK(retry.kind == COG_CLI_CONTROL_UI_COMMAND && retry.wire_id == original_wire);
    unsigned char retried[8192]; size_t retried_bytes;
    OK(cog_cli_control_encode(&retry, retried, sizeof(retried), &retried_bytes));
    CHECK(saved_bytes == retried_bytes && memcmp(saved, retried, saved_bytes) == 0);
    complete(&f, &retry);
    cleanup(&f);
}
static void lost_hello_ack_case(const char *path)
{
    fixture_t f; setup(&f, path, false);
    cog_cli_control_message_t h = next_frame(&f);
    uint64_t original_wire = h.wire_id;
    send_result(&f, &h, COG_OK, 77U); uint64_t result_wire = f.server_wire;
    cog_cli_control_message_t ack = next_frame(&f);
    uint64_t ack_wire = ack.wire_id;
    CHECK(ack.kind == COG_CLI_CONTROL_COMMAND_ACK);
    OK(cog_ipc_endpoint_free(f.peer)); f.peer = NULL;
    h = next_frame(&f);
    CHECK(h.kind == COG_CLI_CONTROL_UI_HELLO && h.wire_id == original_wire && h.runtime_incarnation == 0U);
    f.server_wire = result_wire - 1U; send_result(&f, &h, COG_OK, 77U);
    ack = next_frame(&f); CHECK(ack.kind == COG_CLI_CONTROL_COMMAND_ACK && ack.wire_id == ack_wire);
    send_result(&f, &ack, COG_OK, 0U);
    view(&f, 1U, false);
    cog_cli_control_message_t cur = next_frame(&f); CHECK(cur.kind == COG_CLI_CONTROL_REPLAY);
    cleanup(&f);
}
static void grammar_case(const char *path)
{
    fixture_t f; setup(&f, path, false); online(&f);
    static const struct { const char *text; cog_cli_command_kind_t kind; } cases[] = {
        {"/continue 3 words\n", COG_CLI_COMMAND_CONTINUE}, {"/fork 2\n", COG_CLI_COMMAND_FORK},
        {"/cancel 4\n", COG_CLI_COMMAND_CANCEL}, {"/reload model sample\n", COG_CLI_COMMAND_RELOAD},
        {"/handoff 2 3\n", COG_CLI_COMMAND_HANDOFF}, {"/shutdown\n", COG_CLI_COMMAND_SHUTDOWN}
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        size_t n = strlen(cases[i].text); CHECK(write(f.input[1], cases[i].text, n) == (ssize_t)n);
        cog_cli_control_message_t m = next_frame(&f);
        CHECK(m.kind == COG_CLI_CONTROL_UI_COMMAND);
        cog_cli_command_t cmd; char buf[256]; size_t required;
        OK(cog_cli_command_decode(m.payload, m.payload_len, &cmd, buf, sizeof(buf), &required));
        CHECK(cmd.kind == cases[i].kind && cmd.observed_sequence == 1U);
        complete(&f, &m);
    }
    CHECK(write(f.input[1], "/unknown\n/allow 5\n", 18U) == 18);
    for (size_t i = 0U; i < 80U; ++i) pump(&f);
    CHECK(f.client->final_input_result == COG_ERROR_DENIED);
    CHECK(f.client->requests[0].phase == CLI_CLIENT_REQUEST_ACKED);
    cleanup(&f);
}
static void gap_case(const char *path)
{
    fixture_t f; setup(&f, path, true); online(&f);
    view(&f, 4U, true);
    cog_cli_control_message_t cur = next_frame(&f);
    CHECK(cur.kind == COG_CLI_CONTROL_REPLAY && f.client->rendered_sequence == 4U);
    cog_cli_control_message_t resync = next_frame(&f);
    cog_cli_command_t cmd; char buf[256]; size_t required;
    OK(cog_cli_command_decode(resync.payload, resync.payload_len, &cmd, buf, sizeof(buf), &required));
    CHECK(cmd.kind == COG_CLI_COMMAND_RESYNC && cmd.observed_sequence == 4U);
    complete(&f, &resync);
    CHECK(write(f.input[1], "forbidden\n", 10U) == 10);
    for (size_t i = 0U; i < 40U; ++i) pump(&f);
    CHECK(f.client->final_input_result == COG_ERROR_DENIED);
    cleanup(&f);
}
static void output_failure_case(const char *path)
{
    fixture_t f; setup(&f, path, false); handshake(&f);
    FILE *bad = fopen("/dev/full", "w"); CHECK(bad != NULL);
    CHECK(setvbuf(bad, NULL, _IONBF, 0U) == 0);
    f.client->fixed.output = bad; view(&f, 1U, false);
    cog_result_t rc = COG_OK;
    for (size_t i = 0U; i < 1000U && rc == COG_OK; ++i) {
        uint64_t next; cog_result_t svc = cog_ipc_endpoint_service(f.peer, now(), &next);
        CHECK(svc == COG_OK || svc == COG_ERROR_AGAIN);
        cog_cli_host_runtime_step_t info; rc = cog_cli_host_client_step(f.client, &info);
    }
    CHECK(rc == COG_ERROR_IO && f.client->rendered_sequence == 0U && !f.client->have_view);
    CHECK(cog_cli_host_client_close(&f.client, COG_OK) == COG_ERROR_IO);
    OK(cog_ipc_endpoint_free(f.peer)); OK(cog_ipc_endpoint_free(f.listener));
    CHECK(close(f.input[0]) == 0 && close(f.input[1]) == 0);
    CHECK(fclose(f.output) == 0); (void)fclose(bad);
}

static void error_cleanup(fixture_t *f, cog_result_t expected)
{
    CHECK(cog_cli_host_client_close(&f->client, COG_OK) == expected);
    OK(cog_ipc_endpoint_free(f->peer)); OK(cog_ipc_endpoint_free(f->listener));
    CHECK(close(f->input[0]) == 0 && close(f->input[1]) == 0);
    CHECK(fclose(f->output) == 0);
}
static void identity_case(const char *path)
{
    fixture_t f; setup(&f, path, false); online(&f);
    CHECK(write(f.input[1], "once\n", 5U) == 5);
    cog_cli_control_message_t m = next_frame(&f);
    f.runtime = 43U; send_result(&f, &m, COG_OK, 0U);
    cog_result_t rc = COG_OK;
    for (size_t i = 0U; i < 1000U && rc == COG_OK; ++i) {
        uint64_t next; cog_result_t svc = cog_ipc_endpoint_service(f.peer, now(), &next);
        CHECK(svc == COG_OK || svc == COG_ERROR_AGAIN);
        cog_cli_host_runtime_step_t info; rc = cog_cli_host_client_step(f.client, &info);
    }
    CHECK(rc == COG_ERROR_INDETERMINATE && f.client->runtime == 42U && f.client->lease == 77U);
    error_cleanup(&f, COG_ERROR_INDETERMINATE);
}
static void deadline_case(const char *path, bool known)
{
    fixture_t f; setup(&f, path, false); online(&f);
    CHECK(write(f.input[1], "once\n", 5U) == 5);
    cog_cli_control_message_t m = next_frame(&f);
    if (known) {
        send_result(&f, &m, COG_OK, 0U);
        cog_cli_control_message_t ack = next_frame(&f); CHECK(ack.kind == COG_CLI_CONTROL_COMMAND_ACK);
    }
    /* Inject an expired absolute detach deadline without sleeping or replacing
     * any IPC/clock implementation. */
    f.client->stopping = true; f.client->deadline_ms = 1U;
    cog_cli_host_runtime_step_t info;
    cog_result_t expected = known ? COG_ERROR_IO : COG_ERROR_INDETERMINATE;
    CHECK(cog_cli_host_client_step(f.client, &info) == expected && info.done);
    error_cleanup(&f, expected);
}
static void independent_cancel_case(const char *path)
{
    fixture_t f; setup(&f, path, false); online(&f);
    CHECK(write(f.input[1], "first\n/cancel\n", 14U) == 14);
    cog_cli_control_message_t first = next_frame(&f);
    CHECK(first.kind == COG_CLI_CONTROL_UI_COMMAND && first.lane == COG_CLI_LANE_COMMAND);
    uint64_t first_wire = first.wire_id;
    /* A later rendered view must not replace the read-batch anchor of CANCEL. */
    view(&f, 2U, false);
    cog_cli_control_message_t cancel = next_frame(&f);
    if (cancel.kind == COG_CLI_CONTROL_REPLAY) cancel = next_frame(&f);
    CHECK(cancel.kind == COG_CLI_CONTROL_UI_COMMAND && cancel.lane == COG_CLI_LANE_CANCEL);
    cog_cli_command_t cmd; char buf[256]; size_t required;
    OK(cog_cli_command_decode(cancel.payload, cancel.payload_len, &cmd, buf, sizeof(buf), &required));
    CHECK(cmd.kind == COG_CLI_COMMAND_CANCEL && cmd.observed_sequence == 1U);
    send_result(&f, &cancel, COG_OK, 0U);
    cog_cli_control_message_t ack = next_frame(&f);
    if (ack.kind == COG_CLI_CONTROL_REPLAY) ack = next_frame(&f);
    CHECK(ack.kind == COG_CLI_CONTROL_COMMAND_ACK && ack.lane == COG_CLI_LANE_CANCEL);
    send_result(&f, &ack, COG_OK, 0U);
    for (size_t i = 0U; i < 30U; ++i) pump(&f);
    first.wire_id = first_wire; first.lane = COG_CLI_LANE_COMMAND;
    send_result(&f, &first, COG_ERROR_DENIED, 0U);
    ack = next_frame(&f); CHECK(ack.kind == COG_CLI_CONTROL_COMMAND_ACK);
    send_result(&f, &ack, COG_OK, 0U);
    for (size_t i = 0U; i < 30U; ++i) pump(&f);
    CHECK(f.client->final_input_result == COG_OK && f.client->last_input_serial == 2U);
    cleanup(&f);
}
int main(int argc, char **argv)
{
    (void)test_negative_control(argc, argv);
    CHECK(argc == 2); fixture_t f; setup(&f, argv[1], false); handshake(&f);
    view(&f, 1U, false);
    cog_cli_control_message_t cursor = next_frame(&f);
    CHECK(cursor.kind == COG_CLI_CONTROL_REPLAY && f.client->rendered_sequence == 1U);
    CHECK(write(f.input[1], "hello", 5U) == 5); CHECK(close(f.input[1]) == 0); f.input[1] = -1;
    cog_cli_control_message_t command_frame = next_frame(&f);
    CHECK(command_frame.kind == COG_CLI_CONTROL_UI_COMMAND);
    cog_cli_command_t cmd; char decoded[256]; size_t required;
    OK(cog_cli_command_decode(command_frame.payload, command_frame.payload_len, &cmd, decoded, sizeof(decoded), &required));
    CHECK(cmd.kind == COG_CLI_COMMAND_SUBMIT && cmd.observed_sequence == 1U && strcmp(cmd.text, "hello") == 0);
    CHECK(!f.client->eof || f.client->requests[0].phase == CLI_CLIENT_REQUEST_PENDING);
    send_result(&f, &command_frame, COG_OK, 0U);
    cog_cli_control_message_t ack = next_frame(&f); CHECK(ack.kind == COG_CLI_CONTROL_COMMAND_ACK);
    send_result(&f, &ack, COG_OK, 0U);
    for (size_t i = 0U; i < 1000U && f.client->phase != CLI_CLIENT_DONE; ++i) pump(&f);
    CHECK(f.client->phase == CLI_CLIENT_DONE);
    rewind(f.output); char out[4096]; size_t n = fread(out, 1U, sizeof(out) - 1U, f.output); out[n] = '\0';
    CHECK(strstr(out, "\\u001b\\u000a") != NULL && strchr(out, '\033') == NULL);
    cleanup(&f);
    reconnect_case(argv[1]); lost_hello_ack_case(argv[1]);
    grammar_case(argv[1]); gap_case(argv[1]); output_failure_case(argv[1]);
    identity_case(argv[1]); deadline_case(argv[1], false); deadline_case(argv[1], true);
    independent_cancel_case(argv[1]);
    return 0;
}
