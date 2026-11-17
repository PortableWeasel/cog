/**
 * @file provider_prompt.h
 * @brief Draft provider-neutral instruction/tool input and static template codec.
 *
 * Header-first design, not delivered runtime support. This is a pure preparation
 * surface: it never invokes connect/send/poll/preflight, performs I/O, modifies
 * a provider or authorizes effects. The headless runtime uses the same compiled view
 * for send and all no-wire preflight paths. Existing nine provider callbacks and
 * request ABI are unchanged. Owner thread; no recursive compiler/codec calls.
 */
#ifndef COG_PROVIDER_PROMPT_H
#define COG_PROVIDER_PROMPT_H

#include <cog/provider_vtable.h>
#include <cog/prompt.h>
#include <cog/tool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One complete preparation input, transitively borrowed only by compile. */
typedef struct cog_provider_prompt_input {
    cog_prompt_bundle_t prompt; /**< Borrowed frontend/provider-neutral context. */
    cog_provider_request_t request; /**< History/settings; tools_json MUST be
       NULL/0. All nested bytes are copied into compiler storage on success.
       Known pending-tool history is allowed here; caller chooses the correct
       preflight stage. Only provider preflight/send checks history eligibility. */
} cog_provider_prompt_input_t;

/** @brief Finite nonzero constructor ceilings, including generated wrappers. */
typedef struct cog_provider_prompt_limits {
    size_t max_messages;
    size_t max_fragments;
    size_t max_tools;
    size_t max_json_depth; /**< Strict schema/generated JSON depth preflight. */
    size_t max_context_bytes; /**< Exact emitted logical text/name/schema/opaque
       byte sum, without terminators; NOT a tokenizer/context-window guarantee. */
    size_t max_request_bytes; /**< One aligned output arena incl descriptors,
       copied nested data, generated tool JSON, string terminators and padding. */
    size_t scratch_bytes; /**< Codec scratch, separate from retained outputs. */
} cog_provider_prompt_limits_t;

/**
 * @brief Pure static codec into explicit caller-reserved storage.
 * @param[in] input Valid bounded input, including copied descriptor strings.
 * @param[in] limits Constructor ceilings, unchanged.
 * @param[in,out] scratch Private temporary bytes, suitably aligned, exact cap.
 * @param[in] scratch_capacity Bytes; no hidden allocator or retained scratch ptr.
 * @param[out] storage Aligned output arena, not aliasing inputs/scratch/output.
 * @param[in] capacity Bytes in storage.
 * @param[out] request All arrays/strings/opaque bytes MUST point inside storage.
 * @return OK; INVALID_ARG; PROTOCOL malformed JSON/text; UNSUPPORTED role/tool
 * profile; LIMIT configured output/scratch/context bound. No I/O, allocation,
 * provider calls or external state. Failure exposes no request. The compiler
 * calls this only on its unpublished candidate lane, preserving the old view.
 * @note Preserve every history role, call/result ID, opaque byte, usage field
 * and request setting. Only prepend rendered instruction messages and produce
 * tools_json from effective descriptors. DENY bindings are omitted; ASK stays
 * advertised with decision-required guidance. Escape all generated JSON via a
 * strict bounded encoder; never interpolate raw descriptor strings into JSON.
 * Duplicate tool names fail INVALID_ARG, even when one is denied.
 */
typedef cog_result_t (*cog_provider_prompt_render_fn)(
    const cog_provider_prompt_input_t *input, const cog_provider_prompt_limits_t *limits,
    void *scratch, size_t scratch_capacity, void *storage, size_t capacity,
    cog_provider_request_t *request);

/** @brief Adapter-owned static const descriptor; never dynamically registered. */
typedef struct cog_provider_prompt_template {
    const char *id; /**< Stable nonempty protocol/template identity. */
    uint32_t revision; /**< Nonzero; change when generated semantics change. */
    const cog_provider_vtable_t *provider; /**< Exact selected implementation. */
    size_t required_scratch_bytes;
    cog_provider_prompt_render_fn render;
} cog_provider_prompt_template_t;

typedef struct cog_provider_prompt_compiler cog_provider_prompt_compiler_t;

/**
 * @brief Reserve scratch and TWO output lanes; no network/dependency init.
 * @param[out] out Empty slot; NULL on failure, owned compiler on success.
 * @param[in] format Static descriptor and pointed table/ID live through free.
 * @param[in] limits Nonzero ceilings, scratch at least descriptor requirement.
 * @return OK; NULL_PTR; INVALID_ARG descriptor/limits; LIMIT checked arithmetic;
 * NOMEM. Caller supplies immutable static templates; no inferred model mapping.
 */
cog_result_t cog_provider_prompt_compiler_new(cog_provider_prompt_compiler_t **out,
    const cog_provider_prompt_template_t *format, const cog_provider_prompt_limits_t *limits);

/**
 * @brief Compile and atomically publish one fully copied request view.
 * @param[in,out] compiler Owner, outside callbacks.
 * @param[in] selected Exact endpoint implementation selected by the host.
 * @param[in] input Immutable input, including history; tools_json must be NULL.
 * @param[out] request Borrowed compiler-owned nested data, valid until next
 * SUCCESSFUL compile/free. Failure changes neither request nor previous view.
 * @return OK; NULL_PTR; INVALID_ARG strings/counts/duplicate names;
 * UNSUPPORTED template ID/revision/selected mismatch; PROTOCOL invalid strict input;
 * LIMIT constructor ceilings or checked arithmetic. No runtime allocation.
 * @note Codec runs on candidate storage only; compiler validates its returned
 * ranges, sums and unchanged request settings before publication. A codec
 * contract violation returns PROTOCOL and retains the prior request. Empty
 * fragments introduce no synthetic system message. Existing history is never
 * reordered/coalesced, and opaque continuation is never instruction text.
 * The explicit input ID/revision is checked before invoking render, so a new
 * version on the same provider cannot silently re-render an old snapshot.
 * Model and output-token settings must agree with prompt's selected model and
 * finite ceilings. Logical context uses min(compiler,prompt) byte bounds;
 * runtime separately enforces prompt.max_tool_rounds. No instruction library
 * or UI call is needed to compile a caller-supplied cog_prompt_bundle_t.
 */
cog_result_t cog_provider_prompt_compile(cog_provider_prompt_compiler_t *compiler,
    const cog_provider_vtable_t *selected, const cog_provider_prompt_input_t *input,
    cog_provider_request_t *request);

/**
 * @brief Consume compiler/output/scratch storage; NULL is OK.
 * @param[in,out] compiler Owner, no callback or provider borrowing its view.
 * @return OK. All borrowed request views end; no I/O, callbacks or hidden wait.
 */
cog_result_t cog_provider_prompt_compiler_free(cog_provider_prompt_compiler_t *compiler);

#ifdef __cplusplus
}
#endif
#endif /* COG_PROVIDER_PROMPT_H */
