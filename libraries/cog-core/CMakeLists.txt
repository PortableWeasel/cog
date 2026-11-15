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
    src/session_store.c
)
add_library(cog::core ALIAS cog-core)

# Export as cog::core (matching the build-tree alias) so installed
# consumers use the same target name as in-tree consumers.
set_target_properties(cog-core PROPERTIES EXPORT_NAME core)

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
# cog-result — header-only contract target for <cog/result.h>
# -----------------------------------------------------------------------
#
# cog/result.h is a pure value header (the cog_result_t enum) shared by
# every library.  It is exposed as its own INTERFACE target so consumers
# can take the cog-core include path and the C11 requirement without
# linking the cog-core archive.  This breaks the static-library/export
# cycle that existed when cog-obs linked cog::core while cog-core linked
# cog::obs: cog-obs now links only the header-only cog::result, so the
# target graph is acyclic (cog::core -> cog::obs -> cog::result).
#
# result.h itself is not moved and cog-core's own dependencies are
# unchanged: cog-core still links cog::ipc and cog::obs for the headers
# its public API includes.
add_library(cog-result INTERFACE)
add_library(cog::result ALIAS cog-result)

# Export as cog::result (matching the build-tree alias).
set_target_properties(cog-result PROPERTIES EXPORT_NAME result)

target_include_directories(cog-result
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(cog-result INTERFACE c_std_11)

# -----------------------------------------------------------------------
# Install rules
# -----------------------------------------------------------------------
include(GNUInstallDirs)

install(DIRECTORY include/cog
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# cog-result is exported in its own export set so the installed target
# graph stays acyclic.  cog-ipc and cog-obs reference cog::cog-result, and
# cog-core references cog::cog-ipc / cog::cog-obs; if cog-result lived in
# the cog-core export set, no include order of the installed targets files
# would satisfy every cross-reference (core needs ipc/obs, ipc/obs need
# result).  With a dedicated cog-result-targets.cmake, a consumer loads
# result -> ipc -> obs -> core and every check passes.
install(TARGETS cog-core
    EXPORT cog-core-targets)

install(EXPORT cog-core-targets
    FILE cog-core-targets.cmake
    NAMESPACE cog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog)

install(TARGETS cog-result
    EXPORT cog-result-targets)

install(EXPORT cog-result-targets
    FILE cog-result-targets.cmake
    NAMESPACE cog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog)
