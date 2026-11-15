# Initial cache for Cog's curl dependency. Supply an authenticated, patched
# curl source tree and a separately authenticated c-ares installation.
set(ENABLE_ARES ON CACHE BOOL "Use c-ares" FORCE)
set(ENABLE_THREADED_RESOLVER OFF CACHE BOOL "Never start DNS resolver threads" FORCE)
set(CURL_USE_OPENSSL ON CACHE BOOL "Use OpenSSL" FORCE)
set(BUILD_SHARED_LIBS ON CACHE BOOL "Build shared curl" FORCE)
set(BUILD_STATIC_LIBS OFF CACHE BOOL "Do not build static curl" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "No curl tests" FORCE)
set(BUILD_CURL_EXE OFF CACHE BOOL "No curl executable" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "Disable optional dependency" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "Disable optional dependency" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "Disable optional dependency" FORCE)
