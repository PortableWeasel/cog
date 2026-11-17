/**
 * @file instructions.h
 * @brief Draft immutable agent/skill catalogs and resolved instruction snapshots.
 *
 * Header-first authoring, not an implemented runtime/store/worker integration.
 * Constructors deep-copy all inputs into finite, constructor-owned storage.
 * Query operations do not allocate. Every successful constructor has a matching
 * destructor; creating a replacement is explicit and never mutates a live owner.
 * One owner thread per object; immutable views may be read concurrently only
 * while the caller guarantees the owning object cannot be freed.
 *
 * No filesystem, environment, network, shell, provider or application callbacks
 * occur here. Input provenance/authority comes from the host, never frontmatter.
 * B1 readers supply bytes, but an embedder may supply them directly. Text is
 * UTF-8 without NUL, lengths exclude terminators, and string fields are NUL-
 * terminated. A NULL pointer/count pair is permitted only for zero count.
 */
#ifndef COG_INSTRUCTIONS_H
#define COG_INSTRUCTIONS_H

#include <cog/prompt.h>
#include <cog/result.h>
#include <cog/instruction_source.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Initial bounded metadata profile, not general YAML compatibility. */
#define COG_INSTRUCTION_METADATA_V1 1u
/** @brief In-memory snapshot manifest version; not a disk/wire format. */
#define COG_INSTRUCTION_MANIFEST_V1 1u

/** @brief Source classification supplied by the host/reader operation. */
typedef enum cog_instruction_kind {
    COG_INSTRUCTION_HOST = 0,
    COG_INSTRUCTION_AGENTS,
    COG_INSTRUCTION_AGENT,
    COG_INSTRUCTION_SKILL,
    COG_INSTRUCTION_RESOURCE
} cog_instruction_kind_t;

/** @brief Complete source input; identity is (root_id,path), not body headings. */
typedef struct cog_instruction_source {
    cog_instruction_kind_t kind;
    const char *root_id;
    const char *path; /**< Root-relative normalized path; no absolute path. */
    const char *scope; /**< AGENTS directory; empty for root/global sources. */
    const char *owner_skill; /**< Nonempty for RESOURCE, NULL otherwise. */
    uint32_t precedence; /**< Host-assigned; larger wins catalog name collisions.
       Same-kind/name/equal-precedence duplicates fail rather than depend on scan
       order. All HOST fragments are retained in ascending precedence/path order. */
    const char *text;
    size_t text_bytes;
} cog_instruction_source_t;

/** @brief Explicit nonzero ceilings for parser/catalog/snapshot constructors. */
typedef struct cog_instruction_limits {
    size_t max_sources;
    size_t max_entries;
    size_t max_string_bytes; /**< Each ID/path/metadata scalar including NUL. */
    size_t max_source_bytes; /**< Each complete source, excluding NUL. */
    size_t max_metadata_bytes; /**< Frontmatter per file, excluding delimiters. */
    size_t max_total_bytes; /**< All owned text/names/metadata and terminators. */
    size_t max_fragments;
    size_t max_tools;
    size_t max_active_skills;
    size_t max_resources;
} cog_instruction_limits_t;

/** @brief Catalog entry, immutable and owned by the catalog. */
typedef struct cog_instruction_entry {
    cog_instruction_kind_t kind; /**< AGENT or SKILL only. */
    const char *name;
    const char *description;
    const char *root_id;
    const char *path;
    const char *model_hint; /**< Optional agent model constraint. Host must select
       and validate that exact registered model before snapshot construction;
       this field never selects a model or permits fallback by itself. */
    const char *tool_names; /**< Space-separated exact names, NULL inherits;
       explicit empty agent tools disables all. No command-pattern grants. */
    const char *skill_names; /**< Agent initial skills, NULL absent. */
    size_t context_byte_limit; /**< Agent reduction, 0 inherits host ceiling. */
    uint32_t output_token_limit; /**< Agent reduction, 0 inherits host ceiling. */
    uint32_t tool_round_limit; /**< Agent reduction, 0 inherits host ceiling. */
    unsigned char sha256[32]; /**< Complete original file bytes. */
} cog_instruction_entry_t;

typedef struct cog_instruction_catalog cog_instruction_catalog_t;
typedef struct cog_instruction_snapshot cog_instruction_snapshot_t;

/** @brief Explicit effective host settings; no file may increase these bounds. */
typedef struct cog_instruction_selection {
    uint64_t snapshot_id; /**< Host-supplied nonzero, never reused in its catalog. */
    const char *workspace_root_id;
    const char *scope; /**< Target directory relative to the configured workspace. */
    const char *agent_name; /**< NULL for default role. */
    const char *const *skill_names; /**< Explicit activations; unique names. */
    size_t skill_count;
    const char *model_id; /**< Exact host-selected model; not resolved from hint. */
    const char *template_id;
    uint32_t template_revision;
    size_t max_context_bytes;
    uint32_t max_output_tokens;
    uint32_t max_tool_rounds;
    const cog_prompt_tool_binding_t *bindings; /**< Static descriptors remain
       borrowed through snapshot_free; constructor copies descriptive strings.
       File tool restrictions intersect this set; no name can add a descriptor. */
    size_t binding_count;
} cog_instruction_selection_t;

/** @brief Serializable tool metadata only; contains no code/userdata pointers. */
typedef struct cog_instruction_tool_record {
    const char *name;
    const char *description; /**< NULL permitted iff original descriptor has NULL. */
    const char *input_schema;
    cog_prompt_tool_policy_t policy;
} cog_instruction_tool_record_t;

/**
 * @brief Complete snapshot persistence view, never native serialized memory.
 *
 * The complete bounded catalog source cache plus applicable AGENTS/resource
 * additions is retained, so metadata/activation can be rederived without a
 * source reread. Only selected bodies enter fragments; available_skills is
 * metadata-only MODEL disclosure, not the entire cached catalog. Referenced
 * resources absent from sources cannot be invented or read locally by restore.
 * B7 still needs a bounded native capsule and coordinator resource exchange;
 * a manifest view alone supplies neither a transport nor resource authority.
 * sha256 values are per source/entry; no undefined native-struct hash is used.
 */
typedef struct cog_instruction_manifest {
    uint32_t version;
    uint64_t catalog_generation;
    uint64_t snapshot_id;
    const char *workspace_root_id;
    const char *scope;
    const char *agent_name;
    const char *model_id;
    const char *template_id;
    uint32_t template_revision;
    size_t max_context_bytes;
    uint32_t max_output_tokens;
    uint32_t max_tool_rounds;
    const cog_instruction_source_t *sources;
    const unsigned char (*source_sha256)[32];
    size_t source_count;
    const cog_instruction_entry_t *available_skills;
    size_t available_skill_count;
    const char *const *active_skills; /**< Sorted unique names. */
    size_t active_skill_count;
    const cog_instruction_tool_record_t *tools; /**< Effective enabled set. */
    size_t tool_count;
    const cog_prompt_fragment_t *fragments; /**< Fully resolved ordered text. */
    size_t fragment_count;
} cog_instruction_manifest_t;

/** @brief Restore mode; inspection cannot produce executable request inputs. */
typedef enum cog_instruction_restore_mode {
    COG_INSTRUCTION_EXECUTE = 0,
    COG_INSTRUCTION_INSPECT
} cog_instruction_restore_mode_t;

/**
 * @brief Copy and validate a complete immutable host/agent/skill catalog.
 * @param[out] out Empty slot, NULL on failure, owned catalog on success.
 * @param[in] limits Explicit finite bounds; no defaults or runtime growth.
 * @param[in] generation Host-supplied nonzero identity, not a global counter.
 * @param[in] sources HOST/AGENT/SKILL bytes only; no file I/O or lazy parse.
 * @param[in] count Source count, may be zero.
 * @param[out] error Optional diagnostic borrowing caller input IDs/path on
 * failure; consume it before freeing the source inputs. No source text escapes.
 * @return OK; NULL_PTR; INVALID_ARG kinds/identity/limits/duplicate source;
 * PROTOCOL malformed UTF-8/frontmatter/duplicate fields/name/path agreement;
 * UNSUPPORTED metadata grammar/version/field; LIMIT bounds/arithmetic; NOMEM.
 * @note Profile cog-agent:1 and skill frontmatter support bounded string scalars
 * (plain, single/double quoted, literal/folded blocks) plus a single metadata
 * string map. No tags, anchors, aliases, flow collections, implicit includes,
 * interpolation or executable directives. Unknown fields fail UNSUPPORTED.
 * Agent keys: cog-agent,name,description,model,tools,skills,max-context-bytes,
 * max-output-tokens,max-tool-rounds. Skill keys: name,description,license,
 * compatibility,metadata,allowed-tools. The V1 name subset is ASCII lowercase
 * letters/digits/hyphens, 1..64 bytes, no leading/trailing/consecutive hyphens;
 * matches agent basename or skill directory. description is 1..1024 UTF-8
 * characters; compatibility at most 500. Exact required grammar is in DESIGN.
 * Complete source bodies are cached at construction, but only selected bodies
 * enter the prompt. This implements progressive context disclosure without a
 * hidden reread on activation. Scan order cannot change effective entries.
 */
cog_result_t cog_instruction_catalog_new(cog_instruction_catalog_t **out,
    const cog_instruction_limits_t *limits, uint64_t generation,
    const cog_instruction_source_t *sources, size_t count, cog_instruction_diagnostic_t *error);

/**
 * @brief Borrow effective catalog entries, sorted by kind then UTF-8 name bytes.
 * @param[in] catalog Immutable owner.
 * @param[out] entries Catalog-owned array, NULL if empty.
 * @param[out] count Entry count.
 * @return OK; NULL_PTR. Failure leaves both outputs unchanged.
 * @note Views live through catalog_free; no parse, allocation or effects.
 */
cog_result_t cog_instruction_catalog_view(const cog_instruction_catalog_t *catalog,
    const cog_instruction_entry_t **entries, size_t *count);

/**
 * @brief Resolve one profile/scope/activation into an independently owned snapshot.
 * @param[out] out Empty slot; NULL on failure, owned immutable snapshot on OK.
 * @param[in] catalog Complete immutable generation; unchanged by construction.
 * @param[in] limits Snapshot constructor bounds, finite/nonzero.
 * @param[in] selection Host-selected settings and original static bindings.
 * @param[in] additions AGENTS/RESOURCE sources only, already read by the host.
 * @param[in] count Addition count; may be zero. Every resource names an active
 * skill, has that skill's root_id and a normalized path strictly below its
 * SKILL.md parent directory; no cross-package resources or recursive includes.
 * @param[out] error Optional diagnostic borrowing catalog or addition source
 * identities on failure; those input owners must outlive diagnostic consumption.
 * @return OK; NULL_PTR; INVALID_ARG identity/tool duplicates/zero host bounds;
 * NOT_FOUND agent/skill/resource owner; DENIED profile requiring a tool outside
 * the enabled host set; UNSUPPORTED model-hint mismatch or unsupported exact
 * allowed-tools expression; PROTOCOL source/scope/name inconsistency; LIMIT;
 * NOMEM. No partial snapshot or provider/tool effect on failure.
 * @note AGENTS must be the root-to-scope chain under workspace_root_id, not a
 * sibling subtree; host establishes completeness with a B1 SCOPE read. Resolver
 * cannot infer absent filesystem rules from missing caller bytes. Tool names,
 * descriptions and schemas derive from the same selected static descriptors;
 * denied bindings are omitted, ASK is never converted to ALLOW. Snapshot owns
 * all manifest text and no longer borrows catalog/additions/selection strings.
 * It still borrows code-bearing static descriptors until snapshot_free.
 * Order: HOST, shallow-to-deep AGENTS, agent role, skill metadata, active skills
 * sorted by name, resources sorted by (owner,path), then generated tool guidance.
 * Role-initial skills and explicit skill_names form a deduplicated union;
 * duplicate names within the explicit input are INVALID_ARG. Advertised skill
 * metadata is filtered to capabilities available under the effective bindings;
 * explicit ineligible activation fails rather than silently skipping a skill.
 * Text order expresses source scope, not enforced security authority. Byte
 * exhaustion fails rather than truncates. Catalog generation stays pinned.
 */
cog_result_t cog_instruction_snapshot_new(cog_instruction_snapshot_t **out,
    const cog_instruction_catalog_t *catalog, const cog_instruction_limits_t *limits,
    const cog_instruction_selection_t *selection,
    const cog_instruction_source_t *additions, size_t count, cog_instruction_diagnostic_t *error);

/**
 * @brief Borrow the exact resolved manifest, without parsing or allocation.
 * @param[in] snapshot Immutable owner, including inspection snapshots.
 * @param[out] manifest Snapshot-owned nested views through snapshot_free.
 * @return OK; NULL_PTR. Failure leaves output unchanged. No disk/wire codec or
 * persistence is implied by this view; B7 needs explicit versioned encoding.
 */
cog_result_t cog_instruction_snapshot_view(const cog_instruction_snapshot_t *snapshot,
    cog_instruction_manifest_t *manifest);

/**
 * @brief Deep-copy a validated resolved manifest without rereading source files.
 * @param[out] out Empty slot, NULL on failure, owned snapshot on success.
 * @param[in] limits Explicit finite constructor bounds.
 * @param[in] manifest Borrowed full state/bytes; version must be supported.
 * @param[in] mode EXECUTE validates bindings; INSPECT exposes manifest only.
 * @param[in] bindings Current host static descriptor/effective policy references;
 * descriptions/schemas/names must match retained tools exactly. Host policy
 * must permit the archived policy (ALLOW permits ASK/ALLOW; ASK only ASK;
 * DENY permits neither). The restored effective policy stays archived; extra
 * current host tools are never added to the retained advertisement.
 * @param[in] count Binding count; INSPECT requires NULL/0.
 * @param[out] error Optional diagnostic borrowing the input manifest's source
 * identity on failure, never storage from the failed new object.
 * @return OK; NULL_PTR; INVALID_ARG mode/bounds; PROTOCOL inconsistent or
 * corrupt identities/hashes/fragment/source associations; UNSUPPORTED version,
 * changed tool grammar or descriptor metadata; DENIED current permissions no
 * longer permit the retained set; LIMIT; NOMEM. No source/provider/tool calls.
 * @note Restore never silently rebases model/template/scope/tools. Explicit
 * fresh-input rebase is separate runtime admission, not pending-tool replay.
 * Native structs/pointers are not a wire format. Legacy sessions with no stored
 * manifest need an explicit runtime adoption policy before effect admission.
 */
cog_result_t cog_instruction_snapshot_restore(cog_instruction_snapshot_t **out,
    const cog_instruction_limits_t *limits, const cog_instruction_manifest_t *manifest,
    cog_instruction_restore_mode_t mode, const cog_prompt_tool_binding_t *bindings,
    size_t count, cog_instruction_diagnostic_t *error);

/**
 * @brief Borrow frontend/provider-neutral resolved fragments, tools and bounds.
 * @param[in] snapshot Executable resolved owner; no inspection snapshot.
 * @param[out] bundle Snapshot-owned views, valid through snapshot_free.
 * @return OK; NULL_PTR; INVALID_STATE inspection. Failure leaves bundle unchanged.
 * @note No provider request/history/UI type, callback, allocation or I/O. The
 * headless runtime pairs this view with its selected history/settings, then
 * provider compilation checks model/template identity and full rendered bounds.
 * This query alone grants neither protocol compatibility nor tool admission.
 */
cog_result_t cog_instruction_prompt_view(const cog_instruction_snapshot_t *snapshot,
    cog_prompt_bundle_t *bundle);

/**
 * @brief Consume catalog and cached source/parser storage; NULL is OK.
 * @param[in,out] catalog Owner with no outstanding readers.
 * @return OK. Snapshots own their copied text and do not borrow this catalog.
 */
cog_result_t cog_instruction_catalog_free(cog_instruction_catalog_t *catalog);

/**
 * @brief Consume resolved snapshot storage; NULL is OK.
 * @param[in,out] snapshot Owner after all compiler/runtime borrowers have ended.
 * @return OK. No callbacks, source I/O, provider work or hidden wait.
 */
cog_result_t cog_instruction_snapshot_free(cog_instruction_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
#endif /* COG_INSTRUCTIONS_H */
