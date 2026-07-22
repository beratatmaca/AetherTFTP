# Developer tooling targets: cmake --build build --target {format,format-check,tidy,docs}

file(GLOB_RECURSE AETHER_FORMAT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.h"
    "${CMAKE_SOURCE_DIR}/tests/*.cpp"
    "${CMAKE_SOURCE_DIR}/tests/*.h"
)
file(GLOB_RECURSE AETHER_TIDY_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/tests/*.cpp"
)

find_program(CLANG_FORMAT_EXE NAMES clang-format clang-format-18 clang-format-17)
if(CLANG_FORMAT_EXE)
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXE} -i --style=file ${AETHER_FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Formatting sources with clang-format"
        VERBATIM)
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror --style=file
                ${AETHER_FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking source formatting (clang-format --dry-run)"
        VERBATIM)
else()
    message(STATUS "clang-format not found — 'format' targets disabled")
endif()

find_program(RUN_CLANG_TIDY_EXE NAMES run-clang-tidy run-clang-tidy-18)
find_program(CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-18)
if(RUN_CLANG_TIDY_EXE)
    # run-clang-tidy parallelises across the compilation database.
    add_custom_target(tidy
        COMMAND ${RUN_CLANG_TIDY_EXE} -p "${CMAKE_BINARY_DIR}" -quiet
                ${AETHER_TIDY_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Linting with clang-tidy (run-clang-tidy)"
        VERBATIM)
elseif(CLANG_TIDY_EXE)
    add_custom_target(tidy
        COMMAND ${CLANG_TIDY_EXE} -p "${CMAKE_BINARY_DIR}" ${AETHER_TIDY_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Linting with clang-tidy"
        VERBATIM)
else()
    message(STATUS "clang-tidy not found — 'tidy' target disabled")
endif()

find_package(Doxygen OPTIONAL_COMPONENTS dot)
if(DOXYGEN_FOUND)
    set(DOXYGEN_PROJECT_NAME "AetherTFTP")
    set(DOXYGEN_PROJECT_NUMBER "${PROJECT_VERSION}")
    set(DOXYGEN_PROJECT_BRIEF "Cross-platform TFTP client & server (Qt6 / C++17)")
    set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/docs")
    set(DOXYGEN_GENERATE_HTML YES)
    set(DOXYGEN_GENERATE_LATEX NO)
    set(DOXYGEN_EXTRACT_ALL YES)
    set(DOXYGEN_EXTRACT_PRIVATE YES)
    set(DOXYGEN_EXTRACT_STATIC YES)
    set(DOXYGEN_RECURSIVE YES)
    set(DOXYGEN_QUIET YES)
    set(DOXYGEN_WARN_IF_UNDOCUMENTED NO)
    set(DOXYGEN_SOURCE_BROWSER YES)
    if(TARGET Doxygen::dot)
        set(DOXYGEN_HAVE_DOT YES)
        set(DOXYGEN_DOT_IMAGE_FORMAT svg)
        set(DOXYGEN_CALL_GRAPH YES)
        set(DOXYGEN_CALLER_GRAPH YES)
        set(DOXYGEN_DOT_GRAPH_MAX_NODES 100)
    endif()

    doxygen_add_docs(docs
        "${CMAKE_SOURCE_DIR}/src"
        COMMENT "Generating API documentation with Doxygen: ${CMAKE_BINARY_DIR}/docs/html")
else()
    message(STATUS "Doxygen not found — 'docs' target disabled")
endif()
