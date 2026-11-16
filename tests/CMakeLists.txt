# -----------------------------------------------------------------------
# Test suites
# -----------------------------------------------------------------------

if(BUILD_TESTING)
    add_subdirectory(critic-smoke)
endif()

add_subdirectory(unit)
add_subdirectory(integration)

# Scenarios 01-10 (plus the 05b shutdown regression) link cog::cli and
# share integration/fixtures/aux_core with the orchestrator tests.
if(COG_BUILD_CLI)
    add_subdirectory(scenarios)
endif()

# R18 public embedder: normal install/relocation and a separate CMake project.
add_subdirectory(external-consumer)
