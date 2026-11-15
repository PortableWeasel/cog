/**
 * @file toolkit.h
 * @brief Public umbrella header for cog-toolkit.
 *
 * cog-toolkit is the tool execution framework for Cog.  It prepares tool
 * call contexts, dispatches tools from the static tool table defined in
 * cog-core, validates their results, and collects results from parallel
 * invocations.  A consumer of this library needs only:
 *
 * @code
 * #include "cog/toolkit.h"
 * @endcode
 *
 * All state is caller-allocated and passed by pointer.  The library never
 * allocates implicitly: constructors return heap objects with matching
 * destructors, and execution options are supplied by the caller.
 *
 * Threading model: the toolkit is single-threaded by default.  Each
 * @c cog_toolkit_call_t, @c cog_toolkit_result_t, and
 * @c cog_toolkit_result_collection_t instance should be accessed only on
 * the thread that created it or performed the execution.  When
 * @c cog_toolkit_execute_all is invoked with @c max_parallel greater than
 * 1, worker threads are spawned internally, but the result collection is
 * still returned to and owned by the caller's thread.  Multi-threaded
 * operation beyond this may be layered on top via external locking.
 */
#ifndef COG_TOOLKIT_H
#define COG_TOOLKIT_H

#include <cog/toolkit_call.h>
#include <cog/toolkit_exec.h>
#include <cog/toolkit_result.h>
#include <cog/workspace.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif /* COG_TOOLKIT_H */
