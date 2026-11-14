/**
 * @file main.c
 * @brief Entry point for the process-orchestration PoC.
 *
 * The same binary is used for both roles so the orchestrator can spawn
 * auxiliary cores by re-exec'ing itself:
 *
 *   - `proc-orch` (no arguments) runs the orchestrator (main process);
 *   - `proc-orch worker --id N --fd F --interval MS [...]` runs one
 *     auxiliary-core worker, exec'd by the orchestrator.
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"

#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
    if ((argc >= 2) && (strcmp(argv[1], ORCH_MODE_WORKER) == 0)) {
        return worker_run(argc - 2, argv + 2);
    }

    return orchestrator_run(argc - 1, argv + 1);
}
