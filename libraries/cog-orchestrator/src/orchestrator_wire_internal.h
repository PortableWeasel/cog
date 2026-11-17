/** @file orchestrator_wire_internal.h
 * @brief O3 pure native wire constants, no application control dependency.
 * orchestrator_wire.c alone implements the four public CGW1/CGH1 codecs.
 * Integer/prefix/scalar/full-shape validation precedes capacity/writes; all
 * outputs remain transactional under public orchestrator.h error contracts.
 * No PID, role, process, route, store, allocation or I/O authority is encoded.
 */
#ifndef COG_ORCHESTRATOR_WIRE_INTERNAL_H
#define COG_ORCHESTRATOR_WIRE_INTERNAL_H
#include <cog/orchestrator.h>
#define ORCH_CGW1_BYTES 37U
#define ORCH_CGH1_FIXED_BYTES 62U
#define ORCH_SESSION_BYTES 128U
#define ORCH_CGH1_MAX_BYTES (ORCH_CGH1_FIXED_BYTES + ORCH_SESSION_BYTES - 1U)
/* No shared mutable codec workspace or imported CGL/CGV/CGB application headers.
 * Runtime consumes only the approved public codec functions; static byte/UTF-8/
 * scalar helpers remain file-local to orchestrator_wire.c, not a runtime API. */
#endif
