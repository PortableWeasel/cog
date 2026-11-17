/**
 * @file instruction_source.h
 * @brief Draft bounded, explicit-root reader for instruction files and catalogs.
 *
 * Header-first design under e321ec3; runtime delivery is not yet implemented.
 * One owner thread drives one job at a time. No process cwd changes, environment
 * lookup, network, shell, implicit home search or global state. Construction
 * copies configuration, opens root directories and reserves all job storage;
 * begin/poll/view/cancel never allocate or grow it. Filesystem calls can block
 * in the OS: a byte/work quantum is not a universal I/O-latency guarantee.
 *
 * Root IDs are nonempty UTF-8 logical names without ':' or '/', not filesystem authority supplied
 * by Markdown. Relative paths use '/' and reject absolute paths, empty interior
 * components, '.', '..', NUL, symlinks and nonregular files. The empty path is
 * allowed only to name a root directory for scans/scope. Traversal is relative
 * to opened directory descriptors with no-follow checks in every component.
 * Concurrent writers are not fenced: results identify the exact bytes observed,
 * not an atomic snapshot of an independently changing filesystem. Detected
 * inode/size/mtime changes fail IO; no text is silently truncated or repaired.
 * The initial filesystem profile uses Linux getdents64 with constructor-owned
 * buffers, as the existing workspace search does. It does not create allocating
 * DIR streams in poll. Unsupported platforms return UNSUPPORTED at new.
 */
#ifndef COG_INSTRUCTION_SOURCE_H
#define COG_INSTRUCTION_SOURCE_H

#include <cog/result.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One explicit root; ID/path are nonempty UTF-8 and copied by new. */
typedef struct cog_instruction_source_root {
    const char *id;
    const char *directory; /**< Host-selected absolute directory, never expanded. */
} cog_instruction_source_root_t;

/**
 * @brief Bounded diagnostic metadata, never source text or absolute host paths.
 *
 * Constructor diagnostics borrow validated logical root_id/path from caller
 * inputs or an existing input catalog/manifest. They survive only while that
 * input owner remains alive; no failed-constructor private pointer escapes.
 * Reader error views instead borrow the reader until next successful begin/free.
 * Unknown locations use NULL IDs/path and offset_known=false. Byte offsets
 * refer to the original unnormalized source. On successful construction all
 * fields are zero/NULL. Diagnostic outputs must not overlap input/owner memory.
 * On constructor failure, result equals the returned error code and unknown
 * location fields remain zero/NULL; diagnostics are optional caller storage.
 */
typedef struct cog_instruction_diagnostic {
    cog_result_t result;
    const char *root_id;
    const char *path;
    size_t byte_offset;
    bool offset_known;
} cog_instruction_diagnostic_t;

/** @brief Explicit nonzero constructor bounds; zero is invalid. */
typedef struct cog_instruction_source_limits {
    size_t max_roots;
    size_t max_path_bytes; /**< Includes NUL, per logical ID/path. */
    size_t max_file_bytes; /**< Excludes NUL, per regular text file. */
    size_t max_total_bytes; /**< Includes all retained names/text/terminators. */
    size_t max_items;
    size_t max_depth; /**< Maximum path components below an opened root. */
    size_t work_quantum; /**< Max path/directory/file/sort-comparison units per poll. */
    size_t byte_quantum; /**< Max bytes read/hashed per poll. */
} cog_instruction_source_limits_t;

/** @brief Reader constructor input; arrays and strings borrowed only by new. */
typedef struct cog_instruction_source_config {
    const cog_instruction_source_root_t *roots;
    size_t root_count;
    cog_instruction_source_limits_t limits;
} cog_instruction_source_config_t;

/** @brief Fixed operations; no caller glob language or recursive includes. */
typedef enum cog_instruction_source_operation {
    COG_INSTRUCTION_SOURCE_READ = 0, /**< Read exactly one regular UTF-8/NUL-free file. */
    COG_INSTRUCTION_SOURCE_AGENT_SCAN, /**< Direct '*.md' children, names only. */
    COG_INSTRUCTION_SOURCE_SKILL_SCAN, /**< Direct child directories containing SKILL.md. */
    COG_INSTRUCTION_SOURCE_SCOPE /**< Read AGENTS.md from root through target directory. */
} cog_instruction_source_operation_t;

/** @brief One job; path is file for READ, directory for all other operations. */
typedef struct cog_instruction_source_request {
    cog_instruction_source_operation_t operation;
    const char *root_id;
    const char *path;
    bool optional; /**< Only READ may be optional. Missing returns present=false.
       Missing AGENTS.md along SCOPE is normally skipped; an absent target
       directory or explicitly selected scan directory is NOT_FOUND. */
} cog_instruction_source_request_t;

/** @brief Immutable result owned by reader until next successful begin/free. */
typedef struct cog_instruction_source_item {
    const char *root_id;
    const char *path; /**< Root-relative; no resolved absolute host path. */
    bool present;
    bool content_loaded; /**< False for scan listings and optional absence. */
    const char *text; /**< NULL if !content_loaded; otherwise NUL-terminated. */
    size_t text_bytes; /**< Excludes NUL; empty present content is valid. */
    unsigned char sha256[32]; /**< Exact text bytes, excluding NUL; all zero
       when !content_loaded. Digest is integrity metadata, not authentication. */
} cog_instruction_source_item_t;

typedef struct cog_instruction_source_reader cog_instruction_source_reader_t;

/**
 * @brief Open configured roots and reserve every job/result workspace.
 * @param[out] out Empty output slot; NULL on failure, owned reader on OK.
 * @param[in] config Explicit roots and finite nonzero bounds.
 * @param[out] error Optional caller-owned diagnostic; logical root only for a
 * root-open failure, never the absolute root-directory path.
 * @return OK; NULL_PTR; INVALID_ARG duplicate/invalid root IDs or limits;
 * LIMIT checked bound arithmetic; NOMEM; NOT_FOUND root absent; IO inaccessible;
 * UNSUPPORTED safe descriptor-relative traversal unavailable.
 * @post No scanning, model/tool work or partial owner survives failure.
 */
cog_result_t cog_instruction_source_reader_new(cog_instruction_source_reader_t **out,
    const cog_instruction_source_config_t *config, cog_instruction_diagnostic_t *error);

/**
 * @brief Copy and admit one job without scanning/reading file contents yet.
 * @param[in,out] reader Idle or completed reader; active job refuses.
 * @param[in] request Borrowed only until return; validated/copied atomically.
 * @return OK; NULL_PTR; INVALID_ARG operation/path/optional combination;
 * NOT_FOUND unknown root; LIMIT path reservation; INVALID_STATE active job.
 * @post Failure preserves prior job/results. Success invalidates old views,
 * clears the terminal outcome, and owns a new active job; no runtime allocation.
 */
cog_result_t cog_instruction_source_begin(cog_instruction_source_reader_t *reader,
                                 const cog_instruction_source_request_t *request);

/**
 * @brief Perform at most configured work/byte quanta; never sleep or recurse.
 * @param[in,out] reader Reader with an admitted job.
 * @param[out] done True once the retained outcome is terminal, false otherwise.
 * @return OK for progress/success; NULL_PTR; INVALID_STATE no job; terminal
 * NOT_FOUND required path absent, DENIED escape/symlink/nonregular, PROTOCOL
 * invalid text, LIMIT configured bound, IO checked filesystem/read change, or
 * ABORTED after cancel. Terminal errors repeat without further I/O and expose
 * no partial results. Runtime file descriptors close on completion/failure.
 * @note On NULL_PTR/INVALID_STATE, done is unchanged. Empty scan is successful;
 * AGENT_SCAN considers only names ending in .md, ignoring other names;
 * a matching symlink/nonregular entry fails DENIED. SKILL_SCAN ignores regular
 * files and nonmatching children, rejects symlink children, and checks direct
 * directories for regular no-follow SKILL.md; missing SKILL.md is skipped.
 * Both scans return names only, never content_loaded text. Result paths include
 * the requested directory prefix and, for skills, the SKILL.md suffix.
 * Entries are sorted by unsigned UTF-8 path bytes using reserved/in-place
 * resumable sorting; scope results are shallow to deep. Hidden recursive
 * scanning and filesystem discovery outside roots
 * are forbidden. Per-job aggregate bounds include every retained item.
 */
cog_result_t cog_instruction_source_poll(cog_instruction_source_reader_t *reader, bool *done);

/**
 * @brief Borrow the complete successful result array, without I/O/mutation.
 * @param[in] reader Completed successful job.
 * @param[out] items Immutable reader-owned array, NULL for an empty result.
 * @param[out] count Number of items; READ always has one, even optional absence.
 * @return OK; NULL_PTR; AGAIN active job; INVALID_STATE no job; retained terminal
 * error otherwise. Failure changes neither output. Copy/parse before next begin.
 */
cog_result_t cog_instruction_source_view(const cog_instruction_source_reader_t *reader,
                                const cog_instruction_source_item_t **items, size_t *count);

/**
 * @brief Borrow the retained terminal job failure's logical source location.
 * @param[in] reader Owner of an admitted job.
 * @param[out] error Caller-owned record; pointers borrow reader until next begin/free.
 * @return OK when a terminal failure exists (its result is in error.result);
 * NULL_PTR; AGAIN job active; NOT_FOUND successful job; INVALID_STATE no job.
 * Failure leaves error unchanged. No I/O, allocation, mutation or source text.
 */
cog_result_t cog_instruction_source_error(const cog_instruction_source_reader_t *reader,
    cog_instruction_diagnostic_t *error);

/**
 * @brief Close owned job descriptors and retain ABORTED; completed result wins.
 * @param[in,out] reader Reader owner, outside poll; NULL is NULL_PTR.
 * @return OK if cancelled/already completed; INVALID_STATE no admitted job;
 * IO if close fails after consuming descriptors. No partial result is exposed.
 * @note No hidden drain, file write, tool execution or runtime allocation.
 */
cog_result_t cog_instruction_source_cancel(cog_instruction_source_reader_t *reader);

/**
 * @brief Close job/root descriptors and consume all reader storage once.
 * @param[in,out] reader NULL is OK. Non-NULL consumed on every return.
 * @return OK or IO after consuming cleanup; never retry a consumed alias.
 * @pre Owner thread, no outstanding poll/callback; borrowed views end here.
 */
cog_result_t cog_instruction_source_reader_free(cog_instruction_source_reader_t *reader);

#ifdef __cplusplus
}
#endif
#endif /* COG_INSTRUCTION_SOURCE_H */
