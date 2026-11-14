# -----------------------------------------------------------------------
# PoC 3 — HTTP streaming + connection tracking
# -----------------------------------------------------------------------

find_package(CURL QUIET)

add_executable(http-mock-server
    mock_server.c
)
target_include_directories(http-mock-server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_enable_warnings(http-mock-server)

# The demo links the hand-rolled client unconditionally; the libcurl
# client is only compiled when a usable libcurl is present.
set(HTTP_POC_HAVE_CURL 0)
if(CURL_FOUND)
    set(HTTP_POC_HAVE_CURL 1)
endif()

add_executable(http-demo
    demo.c
    conn.c
    sse.c
    http_common.c
    http_hand.c
)
target_compile_definitions(http-demo PRIVATE
    COG_POC_HAVE_CURL=${HTTP_POC_HAVE_CURL})
if(CURL_FOUND)
    target_sources(http-demo PRIVATE http_curl.c)
    target_link_libraries(http-demo PRIVATE CURL::libcurl)
endif()
target_include_directories(http-demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_enable_warnings(http-demo)

# -----------------------------------------------------------------------
# Demo test: mock server + both transports across the three scenarios.
# -----------------------------------------------------------------------
add_test(NAME http-streaming-demo
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/demo-test.sh
            "$<TARGET_FILE:http-mock-server>"
            "$<TARGET_FILE:http-demo>"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
