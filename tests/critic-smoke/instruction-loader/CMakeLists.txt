cmake_minimum_required(VERSION 3.16)
project(cog_instruction_loader_contract LANGUAGES C)
if(NOT DEFINED COG_SOURCE_DIR)
    message(FATAL_ERROR "COG_SOURCE_DIR is required")
endif()

# Deliberately expose only neutral core headers. UI/provider/net/IPC include
# directories and binary targets do not exist in this consumer project.
add_library(cog::result INTERFACE IMPORTED)
set_target_properties(cog::result PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
    "${COG_SOURCE_DIR}/libraries/cog-core/include")
add_subdirectory("${COG_SOURCE_DIR}/libraries/cog-instructions" instructions)
get_target_property(instruction_dependencies cog-instructions INTERFACE_LINK_LIBRARIES)
if(NOT instruction_dependencies STREQUAL "cog::result")
    message(FATAL_ERROR "Instruction contract gained a non-neutral dependency: ${instruction_dependencies}")
endif()

add_library(instruction-loader-contract OBJECT contract.c)
set_target_properties(instruction-loader-contract PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(instruction-loader-contract PRIVATE cog::instructions)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(instruction-loader-contract PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endif()
