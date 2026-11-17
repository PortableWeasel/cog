/**
 * @file prompt.h
 * @brief Draft provider/UI-neutral instruction fragments and effective tool view.
 *
 * These are borrowed immutable data views, not parsers, a registry or effect
 * authority. They introduce no dependency from core onto instruction loading,
 * provider implementations or frontends. Owners retain every nested string and
 * original static descriptor until the consumer copies the bundle. No mutable
 * globals or implicit initialization exist. Text is UTF-8 without embedded NUL;
 * byte lengths exclude NUL, identity strings are nonempty and NUL-terminated.
 * This header is a design contract; runtime instruction delivery is pending.
 */
#ifndef COG_PROMPT_H
#define COG_PROMPT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Original static descriptor, completed by cog/tool.h when needed. */
typedef struct cog_tool cog_tool_t;

/** @brief Logical provenance, not an LLM role or permission grant. */
typedef enum cog_prompt_origin {
    COG_PROMPT_HOST = 0,
    COG_PROMPT_WORKSPACE,
    COG_PROMPT_AGENT,
    COG_PROMPT_SKILL_CATALOG,
    COG_PROMPT_SKILL,
    COG_PROMPT_RESOURCE,
    COG_PROMPT_TOOL_GUIDANCE
} cog_prompt_origin_t;

/** @brief One already-ordered text fragment. */
typedef struct cog_prompt_fragment {
    cog_prompt_origin_t origin;
    const char *source_id; /**< Logical identity, not an absolute host path. */
    const char *scope; /**< Root-relative directory, empty means root scope. */
    const char *text;
    size_t text_bytes;
} cog_prompt_fragment_t;

/** @brief Advertised behavior; execution still needs normal host admission. */
typedef enum cog_prompt_tool_policy {
    COG_PROMPT_TOOL_ALLOW = 0,
    COG_PROMPT_TOOL_ASK,
    COG_PROMPT_TOOL_DENY
} cog_prompt_tool_policy_t;

/** @brief Reference to the same original static descriptor used by dispatch. */
typedef struct cog_prompt_tool_binding {
    const cog_tool_t *descriptor;
    cog_prompt_tool_policy_t policy;
} cog_prompt_tool_binding_t;

/**
 * @brief Frontend-neutral resolved context, separate from conversation history.
 *
 * Identity is (catalog_generation,snapshot_id) within the explicit owner.
 * Model/template constraints are selected by the headless runtime, not by UI
 * rendering or implicit routing. Numeric ceilings are finite/nonzero. Provider
 * preparation must enforce the minimum of these and its own reservations.
 * max_tool_rounds is enforced by runtime admission, never by prompt text.
 * Pointer/count pairs permit NULL only for zero count; a zero-length fragment
 * may have NULL text. No structure here is a native wire/persistence format.
 */
typedef struct cog_prompt_bundle {
    uint64_t catalog_generation;
    uint64_t snapshot_id;
    const char *model_id;
    const char *template_id;
    uint32_t template_revision;
    size_t max_context_bytes;
    uint32_t max_output_tokens;
    uint32_t max_tool_rounds;
    const cog_prompt_fragment_t *fragments;
    size_t fragment_count;
    const cog_prompt_tool_binding_t *tools;
    size_t tool_count;
} cog_prompt_bundle_t;

#ifdef __cplusplus
}
#endif
#endif /* COG_PROMPT_H */
