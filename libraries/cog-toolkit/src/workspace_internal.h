/** @file workspace_internal.h @brief Private bounded workspace execution state. */
#ifndef COG_WORKSPACE_INTERNAL_H
#define COG_WORKSPACE_INTERNAL_H
#include <cog/workspace.h>
#include "toolkit_json.h"
#include <sys/types.h>
#define COG_WORKSPACE_PATH_BYTES 4096U
#define COG_WORKSPACE_IO_BYTES 4096U
struct cog_workspace {
    int root_fd;
    size_t max_file;
    size_t max_entries;
    size_t quantum;
    size_t borrowers;
    size_t temp_serial;
    bool commands;
    char **environment;
    char *strings;
};
typedef enum workspace_operation { WORKSPACE_READ, WORKSPACE_SEARCH, WORKSPACE_EDIT, WORKSPACE_COMMAND } workspace_operation_t;
typedef struct workspace_directory { int fd; char path[COG_WORKSPACE_PATH_BYTES]; } workspace_directory_t;
typedef struct workspace_report { int status; int shell_status; bool shell_known; } workspace_report_t;
typedef struct workspace_execution {
    cog_workspace_t *workspace;
    cog_toolkit_json_t json;
    workspace_operation_t operation;
    cog_tool_async_config_t config;
    const char *path;
    const char *pattern;
    const char *old_text;
    const char *new_text;
    const char *command;
    char *capture;
    char *file;
    char *replacement;
    size_t file_len;
    size_t write_offset;
    size_t pattern_len;
    size_t old_len;
    size_t new_len;
    size_t replacement_len;
    size_t directory_count;
    size_t directory_index;
    size_t examined;
    workspace_directory_t *directories;
    unsigned char directory_buffer[COG_WORKSPACE_IO_BYTES];
    size_t directory_offset;
    size_t directory_bytes;
    char current_path[COG_WORKSPACE_PATH_BYTES];
    char leaf[COG_WORKSPACE_PATH_BYTES];
    char temporary[96];
    int fd;
    int parent_fd;
    int temporary_fd;
    int control_fd;
    int output_fd;
    int report_fd;
    pid_t helper;
    workspace_report_t report;
    size_t report_bytes;
    uint64_t now;
    uint64_t deadline;
    uint64_t kill_deadline;
    bool clock_set;
    bool started;
    bool done;
    bool cancelling;
    bool kill_sent;
    bool helper_reaped;
    bool output_eof;
    bool report_eof;
    unsigned int phase;
    mode_t mode;
    cog_tool_output_t output;
} workspace_execution_t;
int cog_workspace_open_path(cog_workspace_t *workspace, const char *path, int flags);
int cog_workspace_open_parent(cog_workspace_t *workspace, const char *path, char *leaf);
void cog_workspace_finish(workspace_execution_t *execution, cog_result_t status);
void cog_workspace_capture(workspace_execution_t *execution, const char *bytes, size_t length);
cog_result_t cog_workspace_process_start(workspace_execution_t *execution);
cog_result_t cog_workspace_process_poll(workspace_execution_t *execution);
cog_result_t cog_workspace_process_cancel(workspace_execution_t *execution);
#endif
