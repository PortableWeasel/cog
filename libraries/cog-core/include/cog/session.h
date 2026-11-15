/**
 * @file session.h
 * @brief Hierarchical conversation tree with forking, timing, and billing.
 *
 * A session is a tree of conversation nodes. Each node represents a single
 * message exchange (user prompt, assistant response, tool call, tool result).
 * The tree structure makes forking trivial: create a new branch from any
 * existing node.
 *
 * The active path from root to leaf defines the current conversation context.
 * Multiple active paths are possible (parallel subagents, fan-out).
 *
 * Each node preserves low-level provider data, timing information, and
 * billing/cost data. Cumulative cost and timing can be computed up the tree.
 *
 * Threading model: a session is owned by exactly one core instance and is
 * not thread-safe. All access happens on the thread that runs
 * @c cog_core_run.
 */
#ifndef COG_SESSION_H
#define COG_SESSION_H

#include <cog/event.h>
#include <cog/message.h>
#include <cog/result.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Opaque types                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Opaque conversation session (tree).
 *
 * Created with @c cog_session_new and destroyed with @c cog_session_free.
 */
typedef struct cog_session cog_session_t;

/**
 * @brief Opaque conversation node.
 *
 * A node represents a single message exchange in the conversation tree.
 * Nodes are owned by the session and must not be freed directly.
 */
typedef struct cog_session_node cog_session_node_t;

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Configuration passed to @c cog_session_new.
 */
typedef struct cog_session_config {
    /**
     * @brief Optional session identifier.
     *
     * May be NULL; if NULL the implementation generates a unique id.
     * The string is copied during initialisation.
     */
    const char *id;

    /**
     * @brief Maximum number of nodes retained in the tree.
     *
     * Zero selects 4096. At capacity insertion fails COG_ERROR_LIMIT without
     * pruning or changing the active branch; history is never silently lost.
     */
    size_t max_nodes;
    /** Constructor-reserved payload arena; zero selects 16 MiB. Covers all
     * nested message/tool/opaque/raw/ledger data; no allocation during append,
     * fork, ledger update or history building. Exhaustion fails LIMIT atomically. */
    size_t max_payload_bytes;
    size_t max_tool_calls_per_message; /**< Zero selects 16; fixed ledger slots. */
} cog_session_config_t;

/* ------------------------------------------------------------------ */
/* Timing                                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief Timing information for a single node.
 */
typedef struct cog_timing {
    /**
     * @brief When the request was sent (monotonic ms since session start).
     */
    uint64_t request_sent_ms;

    /**
     * @brief When the first token was received (monotonic ms).
     */
    uint64_t first_token_ms;

    /**
     * @brief When the last token was received (monotonic ms).
     */
    uint64_t last_token_ms;

    /**
     * @brief Total duration in milliseconds.
     */
    uint64_t duration_ms;

    /**
     * @brief Time to first token in milliseconds.
     */
    uint64_t ttft_ms;

    /**
     * @brief Tokens per second (output_tokens / duration_seconds).
     */
    double tokens_per_second;
} cog_timing_t;

/* ------------------------------------------------------------------ */
/* Billing                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Cost information for a single node.
 */
typedef struct cog_cost {
    /**
     * @brief Input tokens consumed by this exchange.
     */
    uint64_t input_tokens;

    /**
     * @brief Output tokens produced by this exchange.
     */
    uint64_t output_tokens;

    /**
     * @brief Cost per input token (from endpoint config).
     */
    double cost_per_input_token;

    /**
     * @brief Cost per output token (from endpoint config).
     */
    double cost_per_output_token;

    /**
     * @brief Total cost for this exchange.
     */
    double total_cost;
    bool tokens_known; /**< False means unavailable, never assume counts are zero. */
    bool total_cost_known; /**< False means unknown, not a zero-cost exchange. */
} cog_cost_t;

/* ------------------------------------------------------------------ */
/* Node data                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Role of the node in the conversation.
 */
typedef enum cog_node_role {
    COG_NODE_SYSTEM = 0,
    COG_NODE_USER,
    COG_NODE_ASSISTANT,
    COG_NODE_TOOL_CALL,
    COG_NODE_TOOL_RESULT,
} cog_node_role_t;

/**
 * @brief A conversation node with full context.
 *
 * This is a read-only view into a node. The session owns the data.
 */
typedef struct cog_node_data_view {
    /**
     * @brief Unique node identifier (for persistence).
     */
    uint64_t node_id;

    /**
     * @brief Parent node identifier (0 for root).
     */
    uint64_t parent_id;

    /**
     * @brief Role of this node.
     */
    cog_node_role_t role;

    /**
     * @brief The message content (may be NULL for tool calls).
     */
    const char *content;

    /**
     * @brief Length of content in bytes.
     */
    size_t content_len;

    /**
     * @brief Tool call name (for COG_NODE_TOOL_CALL).
     */
    const char *tool_name;

    /**
     * @brief Tool call arguments JSON (for COG_NODE_TOOL_CALL).
     */
    const char *tool_args;

    /**
     * @brief Tool result content (for COG_NODE_TOOL_RESULT).
     */
    const char *tool_result;

    /**
     * @brief Raw provider request JSON (preserved for debugging).
     */
    const char *raw_request;

    /**
     * @brief Raw provider response JSON (preserved for debugging).
     */
    const char *raw_response;

    /**
     * @brief Number of streaming chunks received.
     */
    size_t chunk_count;

    /**
     * @brief Timing information.
     */
    cog_timing_t timing;

    /**
     * @brief Cost information.
     */
    cog_cost_t cost;

    /**
     * @brief Model used for this exchange (e.g. "claude-sonnet-4-20250514").
     */
    const char *model;

    /**
     * @brief Provider name (e.g. "anthropic", "openai").
     */
    const char *provider;
    /** Complete structured message view (preferred over legacy text fields).
     * All nested storage is session-owned until reset/free or explicit discard.
     * One assistant node per turn; no durable node for each display delta. */
    const cog_message_t *message;
} cog_node_data_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Create a new session.
 *
 * @param[out] out    Uninitialised pointer; set to the new session on success.
 * @param[in]  config Configuration; may be NULL for defaults.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p out is NULL.
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * @post On success, @p *out is a valid session and the caller owns it.
 * @post On failure, @p *out is NULL and no resources are held.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_session_new(cog_session_t **out,
                              const cog_session_config_t *config);

/**
 * @brief Destroy a session and all its nodes.
 *
 * @param[in,out] session Session to destroy; may be NULL (no-op).
 *
 * @post All storage owned by @p session is released.
 *
 * Threading: not thread-safe.
 */
void cog_session_free(cog_session_t *session);

/**
 * @brief Return the session identifier.
 *
 * @param[in] session Session to query; may be NULL.
 * @return Pointer to a NUL-terminated id string, or NULL if @p session
 *         is NULL.
 *
 * Threading: not thread-safe.
 */
const char *cog_session_id(const cog_session_t *session);

/**
 * @brief Reset a session to its initial empty state.
 *
 * All nodes are freed. The session retains its id.
 *
 * @param[in,out] session Session to reset.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p session is NULL.
 * @return COG_ERROR_INVALID_STATE if the session is active in a running core.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_session_reset(cog_session_t *session);

/**
 * @brief Return true if the session has no nodes.
 *
 * @param[in] session Session to query; may be NULL.
 * @return true if the session is NULL or empty.
 *
 * Threading: not thread-safe.
 */
bool cog_session_is_empty(const cog_session_t *session);

/* ------------------------------------------------------------------ */
/* Tree navigation                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Get the root node of the session.
 *
 * @param[in] session Session to query.
 * @return Root node, or NULL if the session is empty.
 *
 * Threading: not thread-safe.
 */
cog_session_node_t *cog_session_root(const cog_session_t *session);

/**
 * @brief Get the active leaf node (end of current conversation path).
 *
 * @param[in] session Session to query.
 * @return Active leaf node, or NULL if the session is empty.
 *
 * Threading: not thread-safe.
 */
cog_session_node_t *cog_session_active(const cog_session_t *session);

/**
 * @brief Get the parent of a node.
 *
 * @param[in] node Node to query.
 * @return Parent node, or NULL if @p node is the root.
 *
 * Threading: not thread-safe.
 */
cog_session_node_t *cog_node_parent(const cog_session_node_t *node);

/**
 * @brief Get the first child of a node.
 *
 * @param[in] node Node to query.
 * @return First child, or NULL if @p node is a leaf.
 *
 * Threading: not thread-safe.
 */
cog_session_node_t *cog_node_first_child(const cog_session_node_t *node);

/**
 * @brief Get the next sibling of a node.
 *
 * @param[in] node Node to query.
 * @return Next sibling, or NULL if @p node is the last child.
 *
 * Threading: not thread-safe.
 */
cog_session_node_t *cog_node_next_sibling(const cog_session_node_t *node);

/**
 * @brief Get the number of children of a node.
 *
 * @param[in] node Node to query.
 * @return Number of children.
 *
 * Threading: not thread-safe.
 */
size_t cog_node_child_count(const cog_session_node_t *node);

/* ------------------------------------------------------------------ */
/* Node data access                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Get the data for a node.
 *
 * @param[in] node Node to query.
 * @return Pointer to node data, or NULL if @p node is NULL.
 *
 * @note The returned pointer is valid until the session is modified.
 *
 * Threading: not thread-safe.
 */
const cog_node_data_t *cog_node_data(const cog_session_node_t *node);

/* ------------------------------------------------------------------ */
/* Tree mutation                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Add a message as a child of the given parent node.
 *
 * Deep-copies the complete structured message, including call IDs, JSON,
 * opaque bytes and usage into constructor-reserved storage. Validates message
 * structure and branch-local result associations before changing the tree.
 * Each identified assistant call starts a PROPOSED execution ledger entry.
 * Returns INVALID_ARG for invalid association/duplicate result, LIMIT if
 * reserved node/payload/ledger capacity is exhausted; failure is atomic.
 * Creates a new node and appends it to the parent's children. If @p parent
 * is NULL, the message is added as a child of the root (or becomes the root
 * if the session is empty).
 *
 * @param[in,out] session Session to modify.
 * @param[in]     parent  Parent node; may be NULL for root.
 * @param[in]     msg     Message to add; must be non-NULL.
 * @param[out]    out_node Receives the new node; may be NULL.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p session or @p msg is NULL.
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_session_add_message(cog_session_t *session,
                                      cog_session_node_t *parent,
                                      const cog_message_t *msg,
                                      cog_session_node_t **out_node);

/**
 * @brief Append a message to the active branch.
 *
 * Convenience wrapper that adds a message as a child of the current
 * active node. Equivalent to:
 * @code
 * cog_session_add_message(session, cog_session_active(session), msg, out_node);
 * @endcode
 *
 * @param[in,out] session Session to modify.
 * @param[in]     msg     Message to append; must be non-NULL.
 * @param[out]    out_node Receives the new node; may be NULL.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p session or @p msg is NULL.
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_session_append(cog_session_t *session,
                                 const cog_message_t *msg,
                                 cog_session_node_t **out_node);

/**
 * @brief Legacy unidentified tool node for inspection only.
 *
 * New code appends one assistant message with identified tool_calls. This
 * helper cannot invent a provider call ID; its history is not replay-safe.
 *
 * @param[in,out] session   Session to modify.
 * @param[in]     parent    Parent node; may be NULL for root.
 * @param[in]     tool_name Name of the tool being called.
 * @param[in]     tool_args Arguments as JSON string.
 * @param[out]    out_node  Receives the new node; may be NULL.
 * @return COG_OK on success.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_session_add_tool_call(cog_session_t *session,
                                        cog_session_node_t *parent,
                                        const char *tool_name,
                                        const char *tool_args,
                                        cog_session_node_t **out_node);

/**
 * @brief Legacy tool result for inspection only.
 *
 * New code appends a TOOL message with explicit tool_call_id. This helper
 * does not invent missing identity and cannot authorize execution/replay.
 *
 * The new node deep-copies the parent node's tool name (when the parent
 * carries one, e.g. a tool-call node) so history messages retain tool
 * identity.  When the parent has no tool name, the result node's
 * tool_name is NULL.  The tool name is owned by the node and released by
 * cog_session_free / cog_session_reset / discard / prune.
 *
 * @param[in,out] session     Session to modify.
 * @param[in]     parent      Parent node (the tool call); must be non-NULL.
 * @param[in]     tool_result Result content.
 * @param[out]    out_node    Receives the new node; may be NULL.
 * @return COG_OK on success.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_session_add_tool_result(cog_session_t *session,
                                          cog_session_node_t *parent,
                                          const char *tool_result,
                                          cog_session_node_t **out_node);

/**
 * @brief Fork the conversation from a given node.
 *
 * Refuses INVALID_STATE if the selected path has incomplete messages or any
 * unresolved tool ledger; forking never authorizes repeating ancestor effects.
 * Creates a new branch starting from @p node. The forked branch shares
 * all ancestor nodes but has its own children. This is cheap (shared
 * immutable nodes).
 *
 * @param[in,out] session Session to modify.
 * @param[in]     node    Node to fork from.
 * @param[out]    out_node Receives the fork point node on the new branch.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p session or @p node is NULL.
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_session_fork(cog_session_t *session,
                               cog_session_node_t *node,
                               cog_session_node_t **out_node);

/* ------------------------------------------------------------------ */
/* History / context building                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Get the conversation history from root to a given node.
 *
 * Returns the path of messages that should be sent to the LLM as context.
 * Legacy scratch array is constructor-reserved and owned by the session;
 * no allocation occurs here. Views remain valid until next history query,
 * mutation/reset/free. Prefer cog_session_copy_history for caller ownership.
 * Incomplete/unsafe legacy tool history returns INVALID_STATE; no truncation.
 *
 * @param[in]  session  Session to query.
 * @param[in]  node     Leaf node (end of conversation path).
 * @param[out] out_msgs Receives pointer to the message array.
 * @param[out] out_count Receives the number of messages.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if any argument is NULL.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_session_get_history(const cog_session_t *session,
                                      const cog_session_node_t *node,
                                      const cog_message_t **out_msgs,
                                      size_t *out_count);

/**
 * @brief Copy active-path message views into caller-provided array.
 * @param[in] session Session owner; non-NULL.
 * @param[in] node Leaf in this session; NULL selects current active leaf.
 * @param[out] messages Destination array; NULL iff capacity=0.
 * @param[in] capacity Number of message entries available.
 * @param[out] out_count Required entries on success/OVERFLOW.
 * @return COG_OK; OVERFLOW writes count only and no entries; NULL_PTR;
 * INVALID_ARG for foreign node; INVALID_STATE for incomplete/uncorrelated
 * history or unresolved/indeterminate tools. No implicit context pruning.
 * @note No allocation. Array caller-owned; nested views session-owned until
 * mutation/reset/free. Owner thread only. Empty history succeeds with count 0.
 */
cog_result_t cog_session_copy_history(const cog_session_t *session,
                                      const cog_session_node_t *node,
                                      cog_message_t *messages, size_t capacity,
                                      size_t *out_count);

/** @brief Persisted at-most-once execution ledger, keyed by assistant/call ID. */
typedef enum cog_tool_execution_state {
    COG_TOOL_EXECUTION_PROPOSED = 0,
    COG_TOOL_EXECUTION_STARTED,
    COG_TOOL_EXECUTION_SUCCEEDED,
    COG_TOOL_EXECUTION_FAILED,
    COG_TOOL_EXECUTION_DENIED,
    COG_TOOL_EXECUTION_CANCELLED,
    COG_TOOL_EXECUTION_INDETERMINATE
} cog_tool_execution_state_t;

/**
 * @brief Compare-and-transition one identified tool's durable intent state.
 * @param[in,out] session Session with the assistant node.
 * @param[in] assistant Assistant message node in session.
 * @param[in] call_id Nonempty ID belonging to that assistant node.
 * @param[in] expected Required previous state.
 * @param[in] next New state: PROPOSED->STARTED/DENIED, STARTED->SUCCEEDED/
 * FAILED/CANCELLED/INDETERMINATE; all final states immutable.
 * @return COG_OK; NULL_PTR; NOT_FOUND for unknown ID; INVALID_STATE on stale
 * expected, illegal transition or mutation of a fork-shared ancestor.
 * @note Owner thread; no allocation/execution/save. Application MUST durably
 * save STARTED before executing; save failure means do not execute. It saves
 * result+terminal state together before sending the next model request. This
 * is at-most-once admission, not an impossible exactly-once filesystem/process
 * transaction: a crash after STARTED resumes INDETERMINATE and NEVER reruns.
 */
cog_result_t cog_session_tool_transition(cog_session_t *session,
                                         cog_session_node_t *assistant,
                                         const char *call_id,
                                         cog_tool_execution_state_t expected,
                                         cog_tool_execution_state_t next);
/**
 * @brief Query the ledger for one assistant tool.
 * @param[in] session Session owner.
 * @param[in] assistant Assistant node.
 * @param[in] call_id Identified call.
 * @param[out] out_state Snapshot, unchanged on failure.
 * @return COG_OK; NULL_PTR; NOT_FOUND; INVALID_ARG for foreign node.
 * @note Owner thread, no allocation; fork ancestors retain execution identity.
 */
cog_result_t cog_session_tool_state(const cog_session_t *session,
                                    const cog_session_node_t *assistant,
                                    const char *call_id,
                                    cog_tool_execution_state_t *out_state);

/* ------------------------------------------------------------------ */
/* Timing and billing                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Set timing information for a node.
 *
 * Called by the provider layer after a request completes.
 *
 * @param[in,out] session Session containing the node.
 * @param[in]     node    Node to update.
 * @param[in]     timing  Timing data to set.
 * @return COG_OK on success.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_session_set_timing(cog_session_t *session,
                                     cog_session_node_t *node,
                                     const cog_timing_t *timing);

/**
 * @brief Set cost information for a node.
 *
 * Called by the provider layer after a request completes.
 *
 * @param[in,out] session Session containing the node.
 * @param[in]     node    Node to update.
 * @param[in]     cost    Cost data to set.
 * @return COG_OK on success.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_session_set_cost(cog_session_t *session,
                                   cog_session_node_t *node,
                                   const cog_cost_t *cost);

/**
 * @brief Get cumulative cost from root to a given node.
 *
 * Sums the cost of all nodes on the path from root to @p node.
 *
 * @param[in] session Session to query.
 * @param[in] node    Leaf node.
 * @return Cumulative cost, or 0.0 if @p node is NULL.
 *
 * Threading: not thread-safe.
 */
double cog_session_cumulative_cost(const cog_session_t *session,
                                    const cog_session_node_t *node);

/**
 * @brief Get cumulative timing from root to a given node.
 *
 * Sums the timing of all nodes on the path from root to @p node.
 *
 * @param[in]  session Session to query.
 * @param[in]  node    Leaf node.
 * @param[out] out     Receives cumulative timing.
 * @return COG_OK on success.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_session_cumulative_timing(const cog_session_t *session,
                                            const cog_session_node_t *node,
                                            cog_timing_t *out);

/* ------------------------------------------------------------------ */
/* Parent session                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Return the parent session identifier.
 *
 * @param[in] session Session to query; may be NULL.
 * @return Parent session id, or NULL if this is a root session.
 *
 * Threading: not thread-safe.
 */
const char *cog_session_parent_id(const cog_session_t *session);

/**
 * @brief Set the parent session identifier.
 *
 * @param[in,out] session   Session to modify.
 * @param[in]     parent_id Parent session id; copied. May be NULL.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p session is NULL.
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_session_set_parent(cog_session_t *session,
                                     const char *parent_id);

/* ------------------------------------------------------------------ */
/* Node mutation (cancellation support)                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Discard a leaf node from the session tree.
 *
 * Removes the node from its parent's child list and frees its resources.
 * Used when the user cancels an in-flight request: the partial assistant
 * response node is discarded, preserving the rest of the tree.
 *
 * @param[in,out] session Session containing the node.
 * @param[in]     node    Leaf node to discard.  Must have no children.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p session or @p node is NULL.
 * @return COG_ERROR_INVALID_STATE if @p node has children (not a leaf).
 * @return COG_ERROR_NOT_FOUND if @p node is not in this session.
 *
 * @pre @p node is a leaf (no children).
 * @pre @p node was created by @c cog_session_add_message on this session.
 *
 * @post On success, the node is removed from the tree and its memory freed.
 * @post The active node is updated to the discarded node's parent if the
 *       discarded node was active.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_session_discard_node(cog_session_t *session,
                                       cog_session_node_t *node);

#ifdef __cplusplus
}
#endif

#endif /* COG_SESSION_H */
