cmake_minimum_required(VERSION 3.16)
project(cog_installed_embedder C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(GNUInstallDirs)
find_package(cog-provider REQUIRED CONFIG)
# These components currently publish raw target exports, not package configs.
# Threads is their declared external requirement; it is not supplied by Cog.
find_package(Threads REQUIRED)
foreach(component config toolkit orchestrator cli)
    include("${cog-provider_DIR}/../cog/cog-${component}-targets.cmake")
endforeach()
foreach(component result ipc obs core net provider config toolkit orchestrator cli cli-config)
    if(NOT TARGET cog::${component})
        message(FATAL_ERROR "Missing installed Cog target cog::${component}")
    endif()
endforeach()
add_executable(installed_embedder consumer.c)
target_link_libraries(installed_embedder PRIVATE
    cog::cli cog::orchestrator cog::config cog::toolkit cog::provider cog::net)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(installed_embedder PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wsign-conversion
        -Wmissing-prototypes -Wstrict-prototypes -Wundef -Wpointer-arith
        -Wcast-align -Wwrite-strings -Wformat=2 -Werror)
    if(EMBEDDER_ASAN)
        target_compile_options(installed_embedder PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(installed_embedder PRIVATE -fsanitize=address,undefined)
    elseif(EMBEDDER_TSAN)
        target_compile_options(installed_embedder PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(installed_embedder PRIVATE -fsanitize=thread)
    endif()
    target_link_options(installed_embedder PRIVATE -Wl,--wrap=curl_global_init,--wrap=curl_global_cleanup)
endif()
# Record resolved public imports so the driver can reject source/build leaks.
file(GENERATE OUTPUT "imports.txt" CONTENT
    "provider=$<TARGET_FILE:cog::provider>\nnet=$<TARGET_FILE:cog::net>\nconfig=$<TARGET_FILE:cog::config>\ncurl=$<TARGET_FILE:CURL::libcurl>\n")

file(RELATIVE_PATH embedder_library_path
    "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}"
    "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
set_target_properties(installed_embedder PROPERTIES
    INSTALL_RPATH "$ORIGIN/${embedder_library_path}")
install(TARGETS installed_embedder RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
