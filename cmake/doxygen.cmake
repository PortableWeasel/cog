#
# cmake/doxygen.cmake — Doxygen documentation target
#
# Provides:
#   cog_docs  — build documentation with Doxygen
#   docs      — high-level alias (build docs + open in browser)
#   docs-open — build docs and open index.html in the default browser
#
# Usage:
#   include(doxygen)                  # after project() call
#   # or from the command line:
#   cmake -S . -B build -DCOG_BUILD_DOCS=ON
#   cmake --build build --target cog_docs
#
# Requires Doxygen 1.9.8+ and graphviz (dot). The cmake module does not
# hard-fail if they are absent; instead it prints a warning and skips the
# targets.
#

find_program(DOXYGEN_EXECUTABLE NAMES doxygen)
find_program(DOT_EXECUTABLE     NAMES dot)

option(COG_BUILD_DOCS
    "Build Doxygen API documentation (requires Doxygen + Graphviz)" OFF)

if(NOT COG_BUILD_DOCS)
    return()
endif()

if(NOT DOXYGEN_EXECUTABLE)
    message(WARNING
        "COG_BUILD_DOCS=ON but Doxygen was not found. "
        "Install doxygen (apt install doxygen) and retry.")
    return()
endif()

if(NOT DOT_EXECUTABLE)
    message(WARNING
        "COG_BUILD_DOCS=ON but Graphviz dot was not found. "
        "Install graphviz (apt install graphviz) and retry.")
    return()
endif()

# ---------------------------------------------------------------------------
# Configure the Doxyfile — substitute CMake variables into a generated copy.
# ---------------------------------------------------------------------------
set(DOXYFILE_IN  "${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile")
set(DOXYFILE_OUT "${CMAKE_CURRENT_BINARY_DIR}/docs/Doxyfile.cmake")

configure_file("${DOXYFILE_IN}" "${DOXYFILE_OUT}" @ONLY)

# docs/output/ is the Doxygen OUTPUT_DIRECTORY relative to the source tree.
# We use an absolute path so the build step works from any working directory.
set(DOXYGEN_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/docs/output")

# ---------------------------------------------------------------------------
# Custom target: cog_docs
# ---------------------------------------------------------------------------
add_custom_target(cog_docs
    COMMAND "${DOXYGEN_EXECUTABLE}" "${DOXYFILE_OUT}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generating Doxygen API documentation…"
    BYPRODUCTS
        "${DOXYGEN_OUTPUT_DIR}/html/index.html"
        "${DOXYGEN_OUTPUT_DIR}/xml/index.xml"
        "${DOXYGEN_OUTPUT_DIR}/cog.tags"
)

# ---------------------------------------------------------------------------
# Convenience alias target
# ---------------------------------------------------------------------------
add_custom_target(docs
    COMMAND "${CMAKE_COMMAND}" --build .
        --target cog_docs
    COMMAND "${CMAKE_COMMAND}" -E echo ""
    COMMAND "${CMAKE_COMMAND}" -E echo
        "Documentation → ${DOXYGEN_OUTPUT_DIR}/html/index.html"
    COMMAND "${CMAKE_COMMAND}" -E echo ""
    DEPENDS cog_docs
    COMMENT "Building and reporting docs path"
)

# ---------------------------------------------------------------------------
# docs-open: build + open in browser
# ---------------------------------------------------------------------------
if(UNIX)
    find_program(XDG_OPEN xdg-open)
    if(XDG_OPEN)
        add_custom_target(docs-open
            COMMAND "${XDG_OPEN}"
                "${DOXYGEN_OUTPUT_DIR}/html/index.html"
            DEPENDS cog_docs
            COMMENT "Opening documentation in browser…"
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# Install docs alongside the library
# ---------------------------------------------------------------------------
install(DIRECTORY "${DOXYGEN_OUTPUT_DIR}/html/"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    COMPONENT Documentation
)

# ---------------------------------------------------------------------------
# Print a summary
# ---------------------------------------------------------------------------
message(STATUS
    "Doxygen docs:   ${DOXYGEN_EXECUTABLE}")
message(STATUS
    "Graphviz dot:   ${DOT_EXECUTABLE}")
message(STATUS
    "Docs output:    ${DOXYGEN_OUTPUT_DIR}/html/")
message(STATUS
    "Build target:   cmake --build . --target cog_docs")