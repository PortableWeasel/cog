/**
 * @file cli.h
 * @brief Testable cog CLI application: options, app context, and the
 *        single-process prompt flow.
 *
 * The CLI wires the real cog-core / cog-provider / cog-obs / cog-store
 * libraries into a functional single-process agent: a static const
 * dispatch table drives the (IDLE, AWAITING_LLM) state machine, the mock
 * provider (REAL by default, explicit mock for fixtures) is polled fairly,
 * and the session tree records every
 * exchange.  Scenario tests link @c cog::cli and drive this API directly
 * instead of spawning the binary.
 *
 * @section cli_ownership Ownership
 *
 * A @c cog_cli_app_t owns everything it creates: the session, the
 * provider, the obs context, the store, the mock script storage, and the
 * optional IPC listener.  The core borrows the session / provider / obs /
 * store and owns the IPC endpoint once configured, so teardown order is
 * fixed: drain/release executions, workspace, core, provider, obs, store,
 * session, then owned
 * storage.  @c cog_cli_app_free performs exactly that order.
 *
 * @section cli_states States and events
 *
 * The dispatch table is static const and lives in cli.c:
 *
 * | State | Event | Handler |
 * |-------|-------|---------|
 * | IDLE | USER_INPUT | append user message, send to provider, start streaming |
 * | IDLE | CANCEL | no-op (self transition) |
 * | IDLE | SHUTDOWN | request the run loop to stop |
 * | AWAITING_LLM | LLM_CHUNK | receive next chunk, append/print, continue or finish |
 * | AWAITING_LLM | LLM_COMPLETE | close the turn, return to IDLE |
 * | AWAITING_LLM | CANCEL | cancel/drain; retain explicit cancelled turn |
 * | AWAITING_LLM | SHUTDOWN | cancel/drain/reap owned work then stop |
 * | TOOL_POLICY | policy decision | deny or persist STARTED before ALLOW |
 * | TOOL_RUNNING | service/cancel | poll bounded output/reap; commit result+ledger |
 *
 * A retryable provider error (e.g. rate limit) is retried once with a
 * bounded backoff derived from the error's @c retry_after_ms; a
 * non-retryable error ends the turn and is reported on stderr.
 *
 * @section cli_threading Threading
 *
 * The app is single-threaded.  @c cog_cli_app_stop is the only call safe
 * from a signal handler or another thread; it forwards to
 * @c cog_core_stop.
 */
#ifndef COG_CLI_CLI_H
#define COG_CLI_CLI_H

#include <cog/core.h>
#include <cog/obs.h>
#include <cog/provider.h>
#include <cog/net.h>
#include <cog/toolkit_exec.h>
#include <cog/result.h>
#include <cog/session.h>
#include <cog/session_store.h>

#include "cog_cli/mock_provider.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Version string reported by --version. */
#define COG_CLI_VERSION "0.1.0"

/**
 * @brief Mock behaviour selected by --mock-mode.
 */
typedef enum cog_cli_mock_mode {
    COG_CLI_MOCK_STREAM = 0,     /**< Stream the configured chunks. */
    COG_CLI_MOCK_ERROR,          /**< One retryable error, then a chunk. */
    COG_CLI_MOCK_CANCEL,         /**< The stream aborts immediately. */
    COG_CLI_MOCK_RATE_LIMIT,     /**< A 429 retryable error, then a chunk. */
} cog_cli_mock_mode_t;

/** @brief Explicit backend selection; real setup never falls back to mock. */
typedef enum cog_cli_backend {
    COG_CLI_BACKEND_REAL = 0,
    COG_CLI_BACKEND_MOCK
} cog_cli_backend_t;
/** @brief Command/workspace policy, applied to every complete identified call. */
typedef enum cog_cli_tool_policy {
    COG_CLI_TOOL_POLICY_ASK = 0, /**< Interactive prompt; noninteractive denies. */
    COG_CLI_TOOL_POLICY_DENY,
    COG_CLI_TOOL_POLICY_ALLOW /**< Explicit autonomous opt-in, still validates paths/limits. */
} cog_cli_tool_policy_t;
/** @brief Real mode defaults to durable history; disabled mode cannot run tools. */
typedef enum cog_cli_store_mode {
    COG_CLI_STORE_DEFAULT = 0, /**< DURABLE for REAL, DISABLED for MOCK. */
    COG_CLI_STORE_DURABLE,
    COG_CLI_STORE_DISABLED
} cog_cli_store_mode_t;
/**
 * @brief Credential resolver callback; only called during config construction.
 * @param[in,out] arg Host-owned resolver context.
 * @param[in] reference Credential reference, never credential contents.
 * @param[out] output Secret destination; no logging/printing/persistence.
 * @param[in] capacity Available bytes including NUL.
 * @param[out] out_required Required capacity, including NUL.
 * @return OK; OVERFLOW (no writes); NOT_FOUND; DENIED; IO. Constructor never
 * passes the secret as process arguments. Default resolves env:NAME only;
 * other schemes require this explicit callback and never execute a shell.
 * @note Owner thread. Caller clears temporary secret copies before release.
 */
typedef cog_result_t (*cog_cli_credential_read_fn)(void *arg,
                                                  const char *reference,
                                                  char *output, size_t capacity,
                                                  size_t *out_required);

/** @brief Presence bits preserve explicit default/zero values across precedence. */
typedef enum cog_cli_option_bit {
    COG_CLI_OPT_BACKEND = 1u << 0,
    COG_CLI_OPT_MODEL = 1u << 1,
    COG_CLI_OPT_ENDPOINT = 1u << 2,
    COG_CLI_OPT_CREDENTIAL = 1u << 3,
    COG_CLI_OPT_WORKSPACE = 1u << 4,
    COG_CLI_OPT_POLICY = 1u << 5,
    COG_CLI_OPT_OUTPUT_TOKENS = 1u << 6,
    COG_CLI_OPT_TOOL_ROUNDS = 1u << 7,
    COG_CLI_OPT_TURN_MS = 1u << 8,
    COG_CLI_OPT_TOOL_MS = 1u << 9,
    COG_CLI_OPT_TOOL_OUTPUT = 1u << 10,
    COG_CLI_OPT_CONTEXT = 1u << 11,
    COG_CLI_OPT_RETRIES = 1u << 12,
    COG_CLI_OPT_RETRY_MS = 1u << 13,
    COG_CLI_OPT_STORE_MODE = 1u << 14,
    COG_CLI_OPT_STORE_DIR = 1u << 15,
    COG_CLI_OPT_BASE_URL = 1u << 16
} cog_cli_option_bit_t;

/**
 * @brief Explicit session command. Selectors are CLI/host inputs, not config or
 * environment defaults; zero initialization preserves NEW behavior.
 */
typedef enum cog_cli_session_action {
    COG_CLI_SESSION_NEW = 0, /**< New session; an existing explicit ID is refused. */
    COG_CLI_SESSION_LIST, /**< Offline durable IDs; no model/secret/net required. */
    COG_CLI_SESSION_RESUME, /**< Guarded exact restore of source_session_id. */
    COG_CLI_SESSION_FORK, /**< RESUME then select a safe node for the next input. */
    COG_CLI_SESSION_RECOVER /**< Offline recovery-only ownership, never execution. */
} cog_cli_session_action_t;

/**
 * @brief Parsed CLI options.
 *
 * Zero-initialise and pass to @c cog_cli_options_parse, which fills the
 * defaults and applies the command line.  String fields point into the
 * caller's @p argv (or static defaults) and are borrowed; the app copies
 * what it needs.
 */
typedef struct cog_cli_options {
    /** Single prompt to run; NULL selects interactive mode. */
    const char *prompt;
    /** NEW session identifier; NULL generates one. Invalid with another action. */
    const char *session_id;
    cog_cli_session_action_t session_action; /**< NEW by default. --sessions,
       --resume ID, --fork ID and --recover ID select mutually exclusive actions. */
    const char *source_session_id; /**< Required for RESUME/FORK/RECOVER, NULL
       otherwise. Copied during construction; 1..127 UTF-8 bytes, no ASCII controls,
       NUL, slash/backslash, . or ..; store-reserved identities are refused. */
    uint64_t fork_node_id; /**< FORK only: --from-node ID, 0 selects active leaf.
       A node must exist and its entire prefix must be replay-safe. The branch
       stays in the SAME session tree and preserves all other branches/IDs. */
    bool previous_processes_fenced; /**< --fenced records the operator's explicit
       assertion that prior writers are quiescent and old command processes have
       been stopped/reaped or externally fenced. It does NOT verify or stop them.
       Required for RECOVER and for RESUME/FORK adoption of unowned v1/v2 data
       (DENIED if absent); optional for owned RESUME/FORK, invalid for NEW/LIST.
       A live store guard still prevents concurrent owner admission. */
    bool show_usage; /**< --usage prints only reported availability/counts/cost
       for completed turns to stderr; false keeps streamed stdout text-only. */
    /** Durable store directory override; NULL selects the mode-specific default. */
    const char *store_dir;
    cog_cli_store_mode_t store_mode; /**< --store DIR selects DURABLE;
       --no-store selects DISABLED; supplying both flags is INVALID_ARG. */
    /** Log file path; NULL logs to stderr. */
    const char *log_path;
    /** Unix domain socket path; NULL runs single-process. */
    const char *socket_path;
    /** Model identifier sent in provider requests. */
    const char *model_id;
    /** Endpoint name to connect through (default openrouter; mock explicit). */
    const char *endpoint_name;
    const char *base_url; /**< HTTPS OpenRouter base URL; explicit --base-url. */
    /** Comma-separated mock chunks. */
    const char *chunks;
    /** Mock behaviour. */
    cog_cli_mock_mode_t mock_mode;
    /** Custom mock script; overrides @p chunks and @p mock_mode when
     *  non-NULL.  The steps array and its strings are borrowed and must
     *  outlive the app. */
    const cog_mock_script_t *script;
    /** Typed deterministic fixture, MOCK only and mutually exclusive with script.
     * Nested deltas/strings are borrowed until app_free. NULL iff count=0;
     * count is at most 4096, each turn ends in FINISH. The app uses the public
     * cog_mock_provider_new_deltas constructor; REAL rejects these inputs. */
    const cog_provider_delta_t *mock_deltas;
    size_t mock_delta_count;
    /** Message queued for the turn after the current one. */
    const char *queue_text;
    /** Read prompts from stdin (default when @p prompt is NULL). */
    bool interactive;
    /** Enable debug logging. */
    bool verbose;
    cog_cli_backend_t backend; /**< Real default; --mock selects fixture backend. */
    const char *config_path; /**< Optional config tree file. */
    const char *credential_ref; /**< Default env:OPENROUTER_API_KEY; never raw key. */
    const char *workspace_root; /**< Required for workspace tool enablement. */
    cog_cli_tool_policy_t tool_policy;
    cog_cli_credential_read_fn credential_read; /**< Optional non-env resolver. */
    void *credential_arg; /**< Borrowed for config construction only. */
    cog_net_ctx_t *net; /**< Explicit host-created real transport, borrowed until
       app_free. Host initializes dependencies before creating net and cleans
       them up after app_free and cog_net_free. Required for REAL, not MOCK. */
    uint32_t max_output_tokens; /**< Real default 4096, must be finite/nonzero. */
    uint32_t max_tool_rounds; /**< Default 8; exhaustion is LIMIT, no new request. */
    uint32_t max_turn_ms; /**< Default 120000, includes backoff and tools. */
    uint32_t max_tool_ms; /**< Default 30000. */
    size_t max_tool_output_bytes; /**< Default 65536, truncation explicitly recorded. */
    size_t max_context_bytes; /**< Default 1 MiB; never silently drop history. */
    uint32_t max_retries; /**< Default 1 additional attempt, application-owned. */
    uint32_t max_retry_elapsed_ms; /**< Default 10000 within max_turn_ms. */
    uint32_t explicit_options; /**< cog_cli_option_bit_t bits set by parse for
       supplied flags. Programmatic callers set bits for intended overrides;
       otherwise environment/config/defaults apply, even if parse filled defaults. */
} cog_cli_options_t;

/**
 * @brief Opaque CLI application context.
 */
typedef struct cog_cli_app cog_cli_app_t;

/** @brief Visible display state; never provider history or opaque continuation. */
typedef struct cog_cli_display_item {
    uint64_t node_id; /**< Durable node ID; 0 for a not-yet-inserted active aggregate. */
    cog_message_role_t role;
    cog_message_status_t status;
    const char *text;
    size_t text_len;
} cog_cli_display_item_t;
/** @brief Atomic full-display snapshot at one replay cursor boundary. */
typedef struct cog_cli_display_snapshot {
    const char *session_id; /**< Nonempty UTF-8, max 127 bytes; part of identity. */
    uint64_t generation; /**< Nonzero replay lifetime identity. */
    uint64_t through_sequence; /**< Includes all display DATA through this ID. */
    const cog_cli_display_item_t *items;
    size_t item_count;
} cog_cli_display_snapshot_t;
/**
 * @brief View full bounded visible history plus the active/incomplete aggregate.
 * @param[in,out] app Owner thread, outside callbacks; no concurrent app mutation.
 * @param[out] snapshot Constructor-scratch view, unchanged on failure.
 * @return OK; NULL_PTR; NOT_FOUND for store-only LIST; LIMIT if complete snapshot cannot fit reserved app limits.
 * @note Valid until the next mutating app call/service/free. Encode/copy before
 * returning to service. Snapshot and replay anchor are sampled atomically on
 * the owner thread; no append can occur between them. Includes display text
 * from incomplete/unresolved tool rounds even when copy_history refuses replay.
 * Excludes opaque bytes/secrets and never authorizes provider/tool execution.
 * UI rejects any snapshot for a different session ID; replaces visible state
 * and sets cursor to (generation,through_sequence),
 * then requests DATA after it. On another gap repeat snapshot; never skip data
 * silently. App constructors reserve snapshot items/text within context/output
 * limits; exhaustion fails clearly instead of returning a partial snapshot.
 */
cog_result_t cog_cli_app_display_snapshot(cog_cli_app_t *app,
                                          cog_cli_display_snapshot_t *snapshot);
/**
 * @brief Encode one CGD1 full-display snapshot for IPC without native structs.
 * @param[in] snapshot Valid borrowed view. @param[out] output Wire bytes.
 * @param[in] capacity Bytes, output NULL iff zero. @param[out] required Wire bytes.
 * @return OK; OVERFLOW changes required only; NULL_PTR; INVALID_ARG; LIMIT.
 * @note CGD1 magic, session ID length u8 and exact UTF-8 bytes (no NUL),
 * generation/through_sequence u64, item count u32, then each
 * node_id u64, role/status u8, text length u32 and exact UTF-8 bytes. Integers
 * big-endian, no pointers/padding/opaque data. No allocation or app mutation.
 */
cog_result_t cog_cli_display_snapshot_encode(const cog_cli_display_snapshot_t *snapshot,
                                             void *output, size_t capacity,
                                             size_t *required);
/**
 * @brief Decode full display snapshot into caller-owned items and text storage.
 * @param[in] input Wire bytes. @param[in] input_len Exact bytes.
 * @param[out] snapshot Metadata, points into items. @param[out] items Output array.
 * @param[in] item_capacity Entries. @param[out] text Text storage including NULs.
 * @param[in] text_capacity Bytes including copied session ID+NUL and item texts.
 * @param[out] required_items Required entries.
 * @param[out] required_text Required bytes. Destinations may be NULL iff capacity=0.
 * @return OK; OVERFLOW changes required counts only; NULL_PTR; UNSUPPORTED
 * version; PROTOCOL zero generation, invalid role/status/UTF-8/embedded NUL,
 * length or trailing bytes. Outputs
 * independent of input/app lifetime. No allocation, provider replay or effects.
 */
cog_result_t cog_cli_display_snapshot_decode(const void *input, size_t input_len,
                                             cog_cli_display_snapshot_t *snapshot,
                                             cog_cli_display_item_t *items,
                                             size_t item_capacity, char *text,
                                             size_t text_capacity,
                                             size_t *required_items,
                                             size_t *required_text);

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Parse command-line arguments into @p out.
 *
 * Fills defaults first, then applies @p argv.  On @c --help or
 * @c --version the usage/version text is printed to stdout and
 * @c COG_ERROR_ABORTED is returned with @p *out_exit_code set to 0.  On
 * an unknown option or a missing value, an error is printed to stderr
 * and @c COG_ERROR_ABORTED is returned with @p *out_exit_code set to 2.
 *
 * @param[out] out           Receives the parsed options.
 * @param[in]  argc          Argument count (including argv[0]).
 * @param[in]  argv          Argument vector.
 * @param[out] out_exit_code Receives the process exit code when
 *                           @c COG_ERROR_ABORTED is returned; may be
 *                           NULL.
 * @return COG_OK to continue, COG_ERROR_ABORTED when the process should
 *         exit with @p *out_exit_code, COG_ERROR_NULL_PTR if @p out is
 *         NULL.
 */
cog_result_t cog_cli_options_parse(cog_cli_options_t *out,
                                   int argc,
                                   char **argv,
                                   int *out_exit_code);

/**
 * @brief Print usage text.
 *
 * @param[in] stream Destination (stdout or stderr).
 */
void cog_cli_print_usage(FILE *stream);

/**
 * @brief Return a static human-readable name for a result code.
 *
 * @param[in] rc Result code.
 * @return Static NUL-terminated name (never NULL).
 */
const char *cog_cli_result_name(cog_result_t rc);

/* ------------------------------------------------------------------ */
/* Application lifecycle                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Create a CLI application from parsed options.
 *
 * Resolves configuration with CLI > environment > config file > defaults.
 * CLI presence is tracked by explicit_options, including explicit zero retries
 * or ASK policy; config uses
 * dotted providers.openrouter.base-url/credential-ref, models.default,
 * workspace.root, tools.policy and limits.*. Environment names are COG_MODEL,
 * COG_ENDPOINT, COG_CREDENTIAL_REF, COG_WORKSPACE, COG_TOOL_POLICY and
 * COG_MAX_OUTPUT_TOKENS/TOOL_ROUNDS/TURN_MS/TOOL_MS/TOOL_OUTPUT_BYTES/
 * CONTEXT_BYTES/RETRIES/RETRY_ELAPSED_MS (COG_MAX_ prefix on each suffix).
 * Backend config key is client.backend; endpoint-name key is providers.selected.
 * --base-url / COG_BASE_URL / providers.openrouter.base-url independently resolve
 * base_url (default https://openrouter.ai/api/v1); endpoint_name is a registry
 * name, never overloaded as a URL.
 * Store keys are store.mode (durable/disabled) and store.directory; environment
 * COG_STORE_MODE and COG_STORE_DIR. Presence resolves each field separately:
 * an explicit CLI policy ASK or retries=0 wins; absent bits do not override
 * environment/config with parse-filled defaults. Empty/malformed PRESENT values
 * fail, not fall through; unknown option bits fail INVALID_ARG. Backend has no
 * environment override (CLI/config backend then REAL default); --mock sets its
 * explicit backend bit. Config path/host callbacks/net are constructor inputs,
 * not secret environment-discovered pointers.
 * REAL without --store uses the file backend at $XDG_STATE_HOME/cog when that
 * variable is nonempty absolute, otherwise $HOME/.local/state/cog (HOME must
 * be absolute). REAL durable mode enables require_owner, admits a fresh process
 * incarnation/epoch, and holds its guard for the entire session execution.
 * Restart never reuses a dead incarnation or bypasses unresolved effects.
 * Coherent unresolved intent may construct a recovery-only app with an explicit
 * recovery guard; no provider/tool/input work is admitted. In interactive run,
 * an operator must choose evidence-backed reconciliation or acknowledge unknown
 * effects; ALLOW policy never substitutes for that decision. Noninteractive run
 * returns INDETERMINATE without effects. Persisted PROPOSED calls instead
 * resume the current explicit policy/denial phase under normal ownership;
 * they are proven unstarted, never confused with crashed STARTED uncertainty.
 * Cancellation/aborted turns close any unstarted proposals with identified
 * no-effect DENIED results before safe continuation. After successful reconciliation save,
 * release recovery guard and admit/acquire a new normal epoch before work.
 * Missing usable location returns NOT_FOUND; creation/open errors
 * return IO, never silently disable persistence. MOCK defaults DISABLED.
 * Explicit DISABLED allows text conversation only: omit tools from requests,
 * refuse every tool ALLOW regardless of policy and return identified denial for
 * unsolicited calls. No null-backend save can satisfy durable STARTED admission.
 * Existing explicit --store directories remain supported; no cwd-based default.
 * Invalid values fail rather than being ignored. Resolves secret references only
 * during construction, validates the full candidate generation and atomically
 * adopts it. Failure retains no partial app and never falls back to mock.
 * Session actions: NEW retains the collision-refusal contract. RESUME/FORK
 * require DURABLE and source_session_id; load an exact committed checkpoint,
 * restore into current explicit constructor reservations, admit a fresh process
 * incarnation/checked epoch and acquire its matching normal guard before work.
 * RESUME/FORK adoption of unowned v1/v2 data requires the explicit prior-writer
 * quiescence/fencing assertion; it is not verification. Text-only v1 migrates
 * through a temporary require_owner=false store context, whose explicit v2 save
 * must succeed; release that context before fenced activation. Unsafe legacy
 * tools stay UNSUPPORTED. Unowned uncertain data refuses INDETERMINATE rather
 * than bootstrapping an unsafe owner, even with the assertion.
 * A competing live guard returns AGAIN; stale checkpoint/owner fails rather
 * than loading another generation. RESUME never reexecutes completed ancestor
 * calls. Coherent PROPOSED resumes only its current explicit policy phase,
 * before unrelated user/branch/model work. Before admitting the first restored
 * PROPOSED call, the generic provider must preflight the bounded full restored
 * selected prefix with PENDING_TOOLS and current request settings. Compatibility
 * failure performs no tool/start/save of STARTED and never calls provider_send
 * to probe support. Build the selected prefix from public node/message views;
 * copy_history correctly refuses unresolved history and is not bypassed for send.
 * STARTED/INDETERMINATE refuses normal
 * resume with INDETERMINATE, never an automatic recovery downgrade. Incomplete
 * text remains visible and requires K's fresh postfailure input to branch.
 * FORK selects fork_node_id through cog_cli_app_fork; --queue is invalid for
 * startup FORK, and its fresh --prompt/interactive input creates the new branch.
 * All non-NEW actions require DURABLE; DEFAULT becomes DURABLE even for MOCK.
 * LIST constructs store-only metadata state: session/provider/core accessors
 * return NULL, no credential resolver/net/provider/guard or workspace executes.
 * RECOVER requires the explicit fencing assertion, loads uncertain history and
 * acquires only a recovery guard; provider/core remain NULL and no credentials
 * or net are required. It refuses absent uncertainty with INVALID_STATE.
 * LIST calls cog_store_list as an explicit temporary output constructor: its
 * allocation is proportional to enumerated metadata, not a fixed app reservation
 * or a pre-enumeration byte cap. The app frees every returned ID/array. Before
 * writing anything, require at most 1024 IDs and aggregate ID bytes+terminators
 * within max_context_bytes (LIMIT otherwise), and validate each as UTF-8/store-
 * safe 1..127 bytes (PROTOCOL otherwise). No IDs are silently dropped/truncated.
 * Listing reports metadata names, not generation checksum/replay health.
 * Both metadata actions reject prompt/queue inputs; malformed configuration
 * still fails, but missing model/credential does not. A usable durable location
 * is still required. LIST performs no store
 * mutation; RECOVER never grants ordinary execution, even under ALLOW policy.
 * Normal modes build bounded session/provider/tool workspaces, obs/store/IPC and the core.
 * Normal REAL execution requires opts.net with explicit host dependency lifecycle; MOCK alone
 * uses fixture scripts. Secret memory is cleared on failed/successful teardown.  Fails closed: on any error
 * every resource created so far is released and @p *out is NULL.
 *
 * @param[out] out  Uninitialised pointer; set to the new app on success.
 * @param[in]  opts Parsed options; must be non-NULL.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p out or @p opts is NULL.
 * @return COG_ERROR_INVALID_ARG for invalid session selectors/IDs, conflicting
 * fields, disabled store for a non-NEW action or malformed present values.
 * @return COG_ERROR_NOT_FOUND for absent session/node or missing normal-mode
 * endpoint/model/credential reference or usable durable location.
 * @return COG_ERROR_DENIED for refused credential access, unavailable policy
 * or missing required recovery/unowned-adoption fencing assertion.
 * @return COG_ERROR_LIMIT for invalid/exhausted finite limits; UNSUPPORTED for
 * a platform without a lock-free app stop flag or
 * backend capabilities/unknown config fields/unsafe continuation format.
 * @return COG_ERROR_IO if the log file cannot be opened or the IPC
 *         listener cannot bind.
 * @return AGAIN for competing live ownership; INDETERMINATE for unsafe normal
 * resume; INVALID_STATE for recovery without uncertain tools or stale admission;
 * another error code from the underlying libraries.
 *
 * @post On success, @p *out is a valid app owned by the caller; release
 *       with @c cog_cli_app_free.
 */
cog_result_t cog_cli_app_new(cog_cli_app_t **out,
                             const cog_cli_options_t *opts);

/**
 * @brief Destroy a CLI application.
 *
 * @param[in,out] app App to destroy; may be NULL (no-op).
 *
 * @pre app_run has returned and no active tool execution remains; graceful
 * shutdown must cancel/poll/reap before this nonblocking destructor.
 * @post All owned resources are released in the documented order. Borrowed
 * net and host dependency runtime remain the caller's responsibility.  The
 *       app must not be used after this call.
 */
void cog_cli_app_free(cog_cli_app_t *app);

/**
 * @brief Select a replay-safe fork point for the next fresh input in this session.
 * @param[in,out] app Normal execution app, idle and outside callbacks.
 * @param[in] node_id Existing node ID; zero selects current active leaf.
 * @return OK; NULL_PTR; NOT_FOUND; AGAIN for active work or queued input;
 * INVALID_STATE for metadata/recovery app or unsafe/unresolved prefix; LIMIT;
 * INDETERMINATE for frozen admission. No wire/tool effects or runtime growth.
 * @note Owner thread. Uses cog_session_fork without copying/rebinding ancestor
 * IDs/effects. A nonleaf selection does not change the current active leaf;
 * the next freshly submitted user message is explicitly added at that point.
 * Existing durable branches remain intact. No durable branch is created until
 * that message is successfully appended/saved. Pending input cannot be silently
 * reassigned to the new branch, so admission refuses while any input is queued.
 */
cog_result_t cog_cli_app_fork(cog_cli_app_t *app, uint64_t node_id);

/**
 * @brief Explicitly reconcile one uncertain tool under recovery-only ownership.
 * @param[in,out] app Constructed with RECOVER and previous_processes_fenced=true.
 * @param[in] assistant_node_id Exact assistant node containing the call.
 * @param[in] call_id Nonempty identified call ID, borrowed for this call only.
 * @param[in] resolution SUCCEEDED/FAILED/CANCELLED only for an outcome externally
 * verified by the operator; ACKNOWLEDGED explicitly preserves unknown effects.
 * @param[in] evidence Nonempty UTF-8 operator evidence, no secrets/opaque data.
 * @param[in] evidence_len Exact evidence bytes, 1..4095 excluding NUL.
 * @return OK only after matching identified result+audit is saved; NULL_PTR;
 * INVALID_ARG for resolution/evidence; NOT_FOUND; INVALID_STATE for wrong mode,
 * non-indeterminate/shared/already-closed call or a prior failed reconciliation;
 * LIMIT; IO/INDETERMINATE for failed/uncertain persistence. No tool execution.
 * @note Owner thread, outside callbacks. Reconciles ONLY this call through
 * cog_session_reconcile_tool; never applies a blanket outcome to other calls.
 * The operator assertion is not independent proof that old processes are dead.
 * A live owner guard prevents concurrent admission. Evidence-backed resolutions
 * are operator statements; ACKNOWLEDGED must not claim that no effect occurred.
 * Failed save freezes further reconciliation in this app; inspect/restart from
 * durable state, never repeat an effect or claim successful recovery. The guard
 * is retained until app_free. Even after all calls close this app remains
 * recovery-only; a later explicit RESUME admits a new epoch and normal guard
 * before provider/tool/input work. No automatic model continuation occurs.
 */
cog_result_t cog_cli_app_reconcile(cog_cli_app_t *app,
                                   uint64_t assistant_node_id,
                                   const char *call_id,
                                   cog_tool_execution_state_t resolution,
                                   const char *evidence, size_t evidence_len);

/**
 * @brief Run the agent loop until stopped.
 *
 * Enqueues the configured @c --prompt (if any) and @c --queue message
 * (if any) before entering the loop.  In interactive mode the wait hook
 * polls stdin within the bounded idle budget; in single-prompt mode it stops the loop when the turn
 * completes.  May be called again after a stop.
 *
 * LIST constructs/validates the complete temporary metadata ID output described
 * by app_new, writes sorted IDs to stdout, frees the output, and exits with no
 * provider work. This explicit output constructor may return NOMEM/LIMIT.
 * RECOVER noninteractive returns INDETERMINATE. Interactive recovery accepts
 * /tools, /history, /reconcile NODE CALL OUTCOME EVIDENCE and /quit; ordinary
 * text/policy ALLOW never executes. Each reconciliation is an explicit operator
 * decision, persisted through cog_cli_app_reconcile. EOF leaves unresolved
 * intent intact; all reconciled returns OK without automatic execution.
 * Normal interactive mode adds /sessions, /history, /fork NODE and /usage.
 * Output/flush errors return IO; opaque data is never rendered and unavailable
 * usage is never guessed. Fresh input is still required after failure/cancel.
 * Uses generic provider/tool poll/next/cancel/error through the fair service
 * hook, including during queued input and timer backoff. One assistant message
 * is assembled per turn; complete tool call IDs, opaque continuation and usage
 * are saved. No tool starts before terminal validation, schema and policy.
 * Persist STARTED before side effects; result+ledger before next model request.
 * New INDETERMINATE state immediately halts model/tool/child/input admission
 * even while the normal guard remains held. Drain existing work, retain evidence
 * and require explicit recovery; a previously acquired guard is no bypass.
 * Retry only before accepted output/effects, within attempt and elapsed limits;
 * transport never retries. Partial failures persist INCOMPLETE and require an
 * explicit user continuation; no replay of executed or indeterminate tools.
 * After an explicit continuation of an incomplete/cancelled model turn, fork
 * from its last replay-safe ancestor and append the newly authorized user input;
 * preserve the failed aggregate on the old branch. Never relabel a failed wire
 * turn COMPLETE or replay its partial tool/opaque state. An unresolved tool
 * instead requires recovery-only ownership and explicit reconcile_tool before
 * ordinary execution. app_run never performs that operator decision implicitly.
 * Shutdown cancels and services tool process-group drain/reap before freeing.
 * @param[in,out] app App to run.
 * @return COG_OK if the loop exited via stop.
 * @return COG_ERROR_NULL_PTR if @p app is NULL.
 * @return Another error code if dispatch or auto-save fails.
 */
cog_result_t cog_cli_app_run(cog_cli_app_t *app);

/**
 * @brief Request the run loop to exit.
 *
 * Signal-safe: records a constructor-verified lock-free stop flag and forwards
 * to @c cog_core_stop when a core exists. Metadata recovery observes the same
 * flag without a core. app_run drains owned work after the core exits.
 *
 * @param[in,out] app App to stop; may be NULL (no-op).
 */
void cog_cli_app_stop(cog_cli_app_t *app);

/* ------------------------------------------------------------------ */
/* Event injection (scenario harness)                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Submit a user prompt as a @c COG_EVENT_USER_INPUT.
 *
 * @param[in,out] app  App to submit into.
 * @param[in]     text Prompt text; copied into the event queue.
 * @param[in]     len  Length of @p text in bytes.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p app or @p text is NULL.
 * @return COG_ERROR_OVERFLOW if the event queue is full; LIMIT if constructor
 *       prompt storage cannot fit text; INVALID_ARG for invalid UTF-8/NUL;
 * INVALID_STATE for LIST/RECOVER; INDETERMINATE for frozen effect admission;
 * original LIMIT/error while an uninserted visible aggregate freezes admission.
 * @post OK owns copied text through handler completion; failure accepts nothing.
 * No runtime allocation/growth or borrowed pointer into caller text is queued.
 */
cog_result_t cog_cli_app_submit(cog_cli_app_t *app,
                                const char *text,
                                size_t len);

/**
 * @brief Queue a message for the turn after the current one.
 *
 * Retains copied text in a reserved application slot for a later safe turn.
 * Earlier queued input cannot authorize continuation after failure/cancel. It
 * stays retained until fresh explicit postfailure submission authorizes the
 * safe branch. LIST/RECOVER never admit ordinary queued prompts.
 *
 * @param[in,out] app  App to queue into.
 * @param[in]     text Message text; copied.
 * @param[in]     len  Length of @p text in bytes.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p app or @p text is NULL.
 * @return COG_ERROR_OVERFLOW if the pending queue is full; LIMIT for prompt
 * capacity; INVALID_ARG for invalid UTF-8/NUL; INVALID_STATE for LIST/RECOVER;
 * INDETERMINATE or original aggregate error for frozen admission.
 */
cog_result_t cog_cli_app_queue(cog_cli_app_t *app,
                               const char *text,
                               size_t len);

/**
 * @brief Request cancellation of the in-flight turn.
 *
 * Enqueues a @c COG_EVENT_CANCEL; the handler arms provider cancellation
 * and tools while preserving queued deltas; fair service drains one terminal
 * outcome and persists an explicit incomplete/cancelled turn.
 *
 * @param[in,out] app App to cancel into.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p app is NULL.
 * @return COG_ERROR_OVERFLOW if the event queue is full; INVALID_STATE for
 * LIST/RECOVER (there is no executable turn).
 */
cog_result_t cog_cli_app_cancel(cog_cli_app_t *app);

/**
 * @brief Request graceful shutdown.
 *
 * Enqueues a @c COG_EVENT_SHUTDOWN; the handler begins cancel/drain/reap.
 * It stops only after owned work reaches releasable terminal state. LIST/RECOVER
 * instead record the app stop flag directly and return OK without a core event.
 *
 * @param[in,out] app App to shut down.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p app is NULL.
 * @return COG_ERROR_OVERFLOW if the event queue is full.
 */
cog_result_t cog_cli_app_shutdown(cog_cli_app_t *app);

/* ------------------------------------------------------------------ */
/* Accessors (scenario harness)                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Return the core instance.
 *
 * @param[in] app App to query; may be NULL.
 * @return The core, or NULL if @p app is NULL.
 */
cog_core_t *cog_cli_app_core(cog_cli_app_t *app);

/**
 * @brief Return the session instance (owned by the app).
 *
 * @param[in] app App to query; may be NULL.
 * @return The session, or NULL if @p app is NULL.
 */
cog_session_t *cog_cli_app_session(cog_cli_app_t *app);

/**
 * @brief Return the provider instance (owned by the app).
 *
 * @param[in] app App to query; may be NULL.
 * @return The provider, or NULL if @p app is NULL.
 */
cog_provider_t *cog_cli_app_provider(cog_cli_app_t *app);

/**
 * @brief Return the obs context (owned by the app).
 *
 * @param[in] app App to query; may be NULL.
 * @return The obs context, or NULL if @p app is NULL.
 */
cog_obs_t *cog_cli_app_obs(cog_cli_app_t *app);

/**
 * @brief Return the session store (owned by the app).
 *
 * @param[in] app App to query; may be NULL.
 * @return The store, or NULL if @p app is NULL or persistence is
 *         disabled.
 */
cog_store_t *cog_cli_app_store(cog_cli_app_t *app);

/**
 * @brief Return the current state-machine state.
 *
 * @param[in] app App to query; may be NULL.
 * @return The current state id, or zero if @p app is NULL.
 */
cog_state_t cog_cli_app_state(cog_cli_app_t *app);

#ifdef __cplusplus
}
#endif

#endif /* COG_CLI_CLI_H */
