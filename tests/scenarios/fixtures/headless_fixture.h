/** @file headless_fixture.h
 * @brief Test-only bounded REAL host setup, driving and owned-process cleanup.
 *
 * One owner thread and one live fixture per process. This deliberately exposes
 * private runtime state for read-only factual assertions; it grants no authority
 * to fabricate registry transitions, provider events or completion receipts.
 */
#ifndef COG_TEST_HEADLESS_FIXTURE_H
#define COG_TEST_HEADLESS_FIXTURE_H
#include "app_internal.h"
#include "composition_internal.h"

typedef struct headless_fixture {
    cog_cli_options_t options;
    cog_net_config_t transport;
    cog_cli_host_runtime_config_t config;
    cog_cli_host_runtime_t *host;
    char ui_path[4096], worker_path[4096];
    bool curl_ready;
} headless_fixture_t;

/** @brief Initialize defaults without constructing a runtime or doing I/O.
 * @param fixture Zeroed or unused fixture, not currently open.
 * @param paths Six borrowed strings: executable, CA, URL, workspace, store,
 * socket prefix. Kept alive through open. All scratch paths must be in build*.
 * @param workers Actual native workers, including zero (max8).
 * @param children Aggregate child capacity (1..16).
 * @return OK, NULL_PTR, INVALID_ARG or LIMIT on a truncated socket path.
 * @post Caller may adjust options/transport/config before open. Defaults retain
 * O4's REAL/synthetic credential,8192 output cap,32 credits,1 child tool round,
 * 20s turn/15s tool,3s shutdown and bounded network timeouts.
 */
cog_result_t headless_fixture_init(headless_fixture_t *fixture,
    const char *const paths[6],size_t workers,size_t children);

/** @brief Initialize curl, construct and begin; partial ownership stays in
 * fixture for close. @param fixture Initialized fixture. @return Actual error. */
cog_result_t headless_fixture_open(headless_fixture_t *fixture);

/** @brief Monotonic milliseconds; zero on clock failure. */
uint64_t headless_fixture_clock_ms(void);

/** @brief One actual runtime step and bounded <=5ms readiness pause.
 * @param host Actual runtime. @param done Actual done result.
 * @return Runtime error, normalizing only AGAIN to OK; no swallowed errors. */
cog_result_t headless_fixture_step(cog_cli_host_runtime_t *host,bool *done);

/** @brief Drive all configured native workers to authenticated profile-ready.
 * @param fixture Open fixture. @param timeout_ms Fixed finite startup bound.
 * @return OK, first step error, INVALID_STATE if done, or TIMEOUT. */
cog_result_t headless_fixture_ready(headless_fixture_t *fixture,uint32_t timeout_ms);

/** @brief Signal exactly one actual owned unreaped worker, never a group.
 * @param host Actual runtime. @param worker_index Physical worker table index.
 * @param signal_number Signal (e.g. SIGSTOP/SIGCONT/SIGKILL).
 * @return OK, INVALID_STATE unless positive owned live PID is proved by
 * WNOWAIT, INVALID_ARG for bad index, or IO. Does not consume wait status. */
cog_result_t headless_fixture_signal_worker(cog_cli_host_runtime_t *host,
    size_t worker_index,int signal_number);

/** @brief Atomically publish read-only actual registry/placement metadata.
 * @param host Actual runtime. @param path Build-directory destination.
 * @return OK, IO, LIMIT, or actual registry query error. No invented IDs.
 * JSON has root_session and members[{agent,peer,session,status,slot}], sampled
 * from admitted executor slots and cog_agent_{status,session_id}.
 */
cog_result_t headless_fixture_registry(cog_cli_host_runtime_t *host,const char *path);

/** @brief Request shutdown, drive 10s then exact owned kill and 5s reap drain;
 * consume quiescent host and curl, preserving first failure.
 * @param fixture Fixture including partially opened states. Resume explicitly
 * paused workers before calling. Never closes a live runtime after timeout.
 * @param primary Existing failure, retained ahead of cleanup failure.
 * @param complete Optional factual all-drained/all-reaped/host-consumed result.
 * @return Primary or first cleanup error. */
cog_result_t headless_fixture_close(headless_fixture_t *fixture,
    cog_result_t primary,bool *complete);
#endif
