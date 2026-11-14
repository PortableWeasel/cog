# -----------------------------------------------------------------------
# PoC 4 — hierarchical config commit/rollback
# -----------------------------------------------------------------------

add_executable(config-tree-demo
    demo.c
    config_tree.c
)

add_executable(config-tree-test
    tests.c
    config_tree.c
)

foreach(target config-tree-demo config-tree-test)
    target_include_directories(${target} PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
    target_enable_warnings(${target})
endforeach()

# -----------------------------------------------------------------------
# Unit/acceptance test driver
# -----------------------------------------------------------------------
add_test(NAME config-tree-tests
    COMMAND config-tree-test
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
