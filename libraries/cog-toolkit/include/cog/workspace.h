/**
 * @file workspace.h
 * @brief Bounded workspace and process tool composition for a coding client.
 *
 * Threading: one workspace and its execution handles have one owner thread.
 * Constructors own root directory descriptors and configured storage; no global
 * cwd changes. All paths are relative to the opened root. Reject absolute paths,
 * .., embedded NUL and symlinks in every component; descriptor-relative no-follow
 * traversal prevents check/use races. Root/path refusal occurs before effects.
 * Command execution is explicit process permission, not an OS sandbox promise.
 */
#ifndef COG_WORKSPACE_H
#define COG_WORKSPACE_H
#include <cog/result.h>
#include <cog/tool.h>
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct cog_workspace cog_workspace_t;
/** @brief Explicit workspace constructor limits; zero bounds are invalid. */
typedef struct cog_workspace_config {
    const char *root; /**< Copied path to directory opened once at construction. */
    size_t max_file_bytes; /**< File read/edit size bound. */
    size_t max_search_entries; /**< Traversal/search work bound. */
    size_t service_quantum; /**< Maximum traversal/file units per poll. */
    const char *const *environment; /**< Copied NAME=value entries; NULL/count 0
       selects PATH=/usr/bin:/bin and LC_ALL=C.UTF-8, no inherited credentials. */
    size_t environment_count;
    bool enable_commands; /**< Default false; per-call ALLOW still required. */
} cog_workspace_config_t;
/**
 * @brief Open workspace root and reserve bounded traversal/edit workspaces.
 * @param[out] out Owned workspace; NULL on failure.
 * @param[in] config Explicit nonzero bounds and root.
 * @return COG_OK; NULL_PTR; INVALID_ARG; NOMEM; IO for inaccessible root;
 * UNSUPPORTED if safe no-follow descriptor traversal is unavailable.
 * @note No file changes/process execution. Owner thread. Free after every
 * borrowing execution is prepared/terminal and released.
 */
cog_result_t cog_workspace_new(cog_workspace_t **out,
                               const cog_workspace_config_t *config);
/**
 * @brief Release workspace reservation and root descriptors.
 * @param[in,out] workspace NULL is COG_OK.
 * @return COG_OK; INVALID_STATE if any execution still borrows it.
 * @note Owner thread, no child abandonment or hidden wait.
 */
cog_result_t cog_workspace_free(cog_workspace_t *workspace);
/**
 * @brief Static asynchronous read/search/edit/command descriptors with schemas.
 *
 * Names: read_file(path), search(pattern,path), edit_file(path,old,new),
 * run_command(command,cwd). JSON arguments are strict objects with no additional
 * properties; cwd defaults to root, edit requires exactly one match and atomic
 * replacement in the same directory. Inputs and capture are bounded by explicit
 * workspace/execution config. Search is literal and traversal bounded. Command
 * uses /bin/sh -c, inherits a sanitized explicit environment and root-relative
 * working directory, owns its process group, and follows timeout/cancel/reap
 * contracts. No shell is launched when commands disabled or policy denied.
 * user in cog_tool_async_config_t MUST point to a live cog_workspace_t.
 * Each result reports truncation, exit status and possible effects explicitly.
 * The application may choose descriptors from this static table; no registration.
 */
extern const cog_tool_table_t cog_workspace_tools;
/** @brief Exact opt-in asynchronous command descriptor for background jobs.
 * Name start_background; identical command/cwd schema and environment, root,
 * process-group and descendant ownership rules as run_command. The runtime
 * wraps this descriptor in guarded job admission. Its output and exit facts
 * remain the execution's terminal result. Standalone blocking paths refuse it.
 *
 * Linux V1 creates the helper and its CLOEXEC pidfd atomically (CLONE_PIDFD).
 * Reserve the control/output/report channels before helper creation; no fallible
 * parent acquisition may remain after a successful spawn. The helper cannot
 * start a command without an established control channel. Lack of atomic
 * pidfd support returns UNSUPPORTED without creating a helper; no numeric-PID
 * fallback. Report UNSUPPORTED/IO as a verified no-effect failure of this
 * descriptor's start. The numeric PID is diagnostic only. Cancellation
 * sends TERM/KILL controls to the helper supervising its owned process group.
 * Parent never signals a detached group using a stored numeric PID. Reap the
 * exact helper once using its pidfd (waitid P_PIDFD), drain bounded output
 * and report channels, and close the
 * pidfd before publishing terminal status. No PID/fd enters model or store data.
 */
extern const cog_tool_t cog_workspace_start_background_tool;
#ifdef __cplusplus
}
#endif
#endif /* COG_WORKSPACE_H */
