# cog-net links a host-initialized curl runtime; the dependency must be audited.
include("${PROJECT_SOURCE_DIR}/cmake/cog_curl.cmake")
find_package(CURL MODULE REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(cog-net STATIC src/net.c)
add_library(cog::net ALIAS cog-net)
set_target_properties(cog-net PROPERTIES EXPORT_NAME net)
target_include_directories(cog-net PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/libraries/cog-core/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(cog-net PUBLIC c_std_11)
target_link_libraries(cog-net PUBLIC CURL::libcurl OpenSSL::Crypto ${CMAKE_DL_LIBS})
target_enable_warnings(cog-net)

# The caller must identify the actual configured build and its shared library.
# Without this proof the runtime remains unavailable rather than accepting an
# unknown or hybrid resolver. No build directory appears in installed exports.
if(COG_NET_CURL_BUILD_DIR OR COG_NET_CURL_LIBRARY)
    if(NOT EXISTS "${COG_NET_CURL_BUILD_DIR}/CMakeCache.txt" OR NOT EXISTS "${COG_NET_CURL_LIBRARY}")
        message(FATAL_ERROR "Cog curl provenance requires a build cache and its shared library")
    endif()
    file(STRINGS "${COG_NET_CURL_BUILD_DIR}/CMakeCache.txt" _ares REGEX "^ENABLE_ARES:BOOL=ON$")
    file(STRINGS "${COG_NET_CURL_BUILD_DIR}/CMakeCache.txt" _threaded REGEX "^ENABLE_THREADED_RESOLVER:BOOL=OFF$")
    get_filename_component(_build_dir "${COG_NET_CURL_BUILD_DIR}" REALPATH)
    get_filename_component(_library "${COG_NET_CURL_LIBRARY}" REALPATH)
    string(FIND "${_library}" "${_build_dir}/" _prefix)
    if(NOT _ares OR NOT _threaded OR NOT _prefix EQUAL 0 OR NOT _library MATCHES "\\.so(\\.[0-9.]+)?$")
        message(FATAL_ERROR "Cog requires a shared curl produced inside a c-ares-only, no-thread build")
    endif()
    file(SHA256 "${_library}" _curl_sha256)
    target_compile_definitions(cog-net PRIVATE COG_NET_CURL_SHA256="${_curl_sha256}")
endif()

if(BUILD_TESTING)
    add_executable(test_net ${PROJECT_SOURCE_DIR}/tests/unit/test_net.c)
    target_link_libraries(test_net PRIVATE cog::net)
    target_enable_warnings(test_net)
    if(COG_NET_CURL_BUILD_DIR AND COG_NET_CURL_LIBRARY)
        target_compile_definitions(test_net PRIVATE COG_NET_TEST_PROVEN)
    endif()
    add_test(NAME unit-net COMMAND test_net)
    if(COG_NET_CURL_BUILD_DIR AND COG_NET_CURL_LIBRARY)
        add_executable(test_net_oom ${PROJECT_SOURCE_DIR}/tests/unit/test_net_oom.c)
        target_link_libraries(test_net_oom PRIVATE cog::net)
        target_enable_warnings(test_net_oom)
        add_test(NAME unit-net-oom COMMAND test_net_oom)
        add_executable(test_net_cog_oom ${PROJECT_SOURCE_DIR}/tests/unit/test_net_cog_oom.c)
        target_link_libraries(test_net_cog_oom PRIVATE cog::net)
        target_link_options(test_net_cog_oom PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc)
        # Allocator linker wrappers invalidate the corresponding builtin effects.
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(test_net_cog_oom PRIVATE
                -fno-builtin-malloc -fno-builtin-calloc)
        endif()
        target_enable_warnings(test_net_cog_oom)
        add_test(NAME unit-net-cog-oom COMMAND test_net_cog_oom)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME unit-net-https COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_https.py $<TARGET_FILE:test_net>)
        add_executable(test_net_regressions ${PROJECT_SOURCE_DIR}/tests/unit/test_net_regressions.c)
        target_link_libraries(test_net_regressions PRIVATE cog::net)
        target_enable_warnings(test_net_regressions)
        add_executable(test_net_observer ${PROJECT_SOURCE_DIR}/tests/unit/test_net_observer.c)
        target_link_libraries(test_net_observer PRIVATE cog::net)
        target_include_directories(test_net_observer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_options(test_net_observer PRIVATE
            -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=curl_easy_getinfo)
        target_compile_options(test_net_observer PRIVATE
            -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc)
        target_enable_warnings(test_net_observer)
        add_test(NAME unit-net-observer COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_observer.py $<TARGET_FILE:test_net_observer>)
        set_tests_properties(unit-net-observer PROPERTIES TIMEOUT 30)
        add_executable(test_net_observer_critic ${PROJECT_SOURCE_DIR}/tests/unit/test_net_observer_critic.c)
        target_link_libraries(test_net_observer_critic PRIVATE cog::net)
        target_include_directories(test_net_observer_critic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_options(test_net_observer_critic PRIVATE -Wl,--wrap=curl_multi_timeout)
        target_enable_warnings(test_net_observer_critic)
        add_test(NAME unit-net-observer-critic COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_observer_critic.py $<TARGET_FILE:test_net_observer_critic>)
        set_tests_properties(unit-net-observer-critic PROPERTIES TIMEOUT 30)
        foreach(_regression completion completion-cancel error-cancel replay auth redirect proxy)
            add_test(NAME unit-net-${_regression}-regression COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_regressions.py
                $<TARGET_FILE:test_net_regressions> ${_regression})
        endforeach()
    endif()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(DIRECTORY include/cog DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS cog-net EXPORT cog-net-targets)
install(EXPORT cog-net-targets FILE cog-net-targets.cmake NAMESPACE cog:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog-net)
configure_package_config_file(cmake/cog-net-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/cog-net-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog-net)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cog-net-config.cmake" DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cog-net)
