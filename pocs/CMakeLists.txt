# -----------------------------------------------------------------------
# Proof-of-concept builds
# -----------------------------------------------------------------------

option(COG_BUILD_POC_IPC_FABRIC "Build PoC 1: IPC fabric over Unix domain sockets" OFF)
option(COG_BUILD_POC_HTTP_STREAMING "Build PoC 3: HTTP streaming + connection tracking" OFF)
option(COG_BUILD_POC_PROC_ORCH "Build PoC 2: process orchestration" OFF)

if(COG_BUILD_POC_IPC_FABRIC)
    add_subdirectory(ipc-fabric)
endif()

if(COG_BUILD_POC_HTTP_STREAMING)
    add_subdirectory(http-streaming)
endif()

if(COG_BUILD_POC_PROC_ORCH)
    add_subdirectory(proc-orch)
endif()
