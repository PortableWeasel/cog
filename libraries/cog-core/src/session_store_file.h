/** @file session_store_file.h @brief Private file-backend absence proof. */
#ifndef COG_SESSION_STORE_FILE_H
#define COG_SESSION_STORE_FILE_H
#include <cog/result.h>
#include <stdbool.h>

/** Recognized v1 names in one session directory, independent of object type. */
typedef struct cog_store_legacy_files {
    bool present; /**< Any metadata, nodes, index or their legacy .tmp names. */
    bool metadata; /**< The authoritative meta.json name itself exists. */
} cog_store_legacy_files_t;

/** Inspect the six fixed legacy names using no-follow directory-relative stat.
 * @param dir Borrowed open session directory descriptor; not closed or changed.
 * @param out Set only on OK. Any object, including a dangling symlink, counts.
 * @return OK, NULL_PTR, or IO for inspection errors other than ENOENT.
 * No allocations, file contents, mutation or directory scan. Caller holds the
 * session lock for admission/publication; diagnostic loads remain nonmutating.
 * This probe applies only when there is no authoritative nonzero v2 checkpoint.
 * meta.json, nodes.jsonl, index.bin and each corresponding .tmp name count.
 * Unreferenced generation-*.bin, .generation.tmp, .manifest.tmp and unrelated
 * files do not establish legacy authority and are not inspected here.
 */
cog_result_t cog_store_file_legacy_probe(int dir, cog_store_legacy_files_t *out);
#endif
