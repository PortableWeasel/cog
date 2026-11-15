# -----------------------------------------------------------------------
# cog-core — agent loop, session state, event queue, and state-machine dispatch
# -----------------------------------------------------------------------

# Stage 12.1e: cog-core is now a real static library.  It compiles the
# Phase 1 modules (session.c, event.c, state.c) into a single archive while
# preserving the public include path, the C11 compile feature, the
# cog::core alias, and the strict-warning settings that downstream
# INTERFACE consumers (cog-provider, cog-toolkit) rely on.
#
# handler.h remains header-only: it declares the handler result type and
# the cog_handler_fn signature but owns no implementation unit, so there is
# deliberately no handler.c.
add_library(cog-core STATIC
    src/session.c
    src/event.c
    src/state.c
    src/core.c
    src/agent.c
    src/tool.c
)
add_library(cog::core ALIAS cog-core)

target_include_directories(cog-core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-core PUBLIC c_std_11)

# cog-core headers include cog-ipc and cog-obs headers (e.g. <cog/ipc.h>
# from event.h and <cog/obs.h> from core.h), so the core propagates both
# include paths to its consumers.  cog-ipc and cog-obs are INTERFACE
# targets, so this adds no link step and the dependency graph stays acyclic
# in practice.
target_link_libraries(cog-core PUBLIC cog::ipc cog::obs)

# Apply the project-wide strict-warning policy (and ASan/UBSan when enabled)
# to the production sources.
target_enable_warnings(cog-core)

# -----------------------------------------------------------------------
# Install rules
# -----------------------------------------------------------------------
include(GNUInstallDirs)

install(DIRECTORY include/cog
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS cog-core
    EXPORT cog-core-targets)

install(EXPORT cog-core-targets
    FILE cog-core-targets.cmake
    NAMESPACE cog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog)
