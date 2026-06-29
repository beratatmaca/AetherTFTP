# ---------------------------------------------------------------------------
# Version resolution.
#
# The project version is MAJOR.MINOR.PATCH.BUILD where:
#   * MAJOR.MINOR.PATCH comes from the top-level VERSION file (the single,
#     human-managed semantic base — bump it for real releases).
#   * BUILD is the git commit count (`git rev-list --count HEAD`), which
#     increases monotonically with every commit/merge to main, giving each
#     build a unique, auto-incrementing version without manual edits.
#
# CI injects the build number and short SHA so every matrix job and the
# GitHub release page share the exact same version:
#   -DAETHER_BUILD_NUMBER=<n>  -DAETHER_GIT_SHA=<sha>
#
# Resolution order (most specific wins): -D override, then git, then a safe
# fallback (0 / "unknown") for source tarballs with no git metadata.
#
# Outputs (set in the including scope):
#   AETHER_VERSION_MAJOR / _MINOR / _PATCH / _BUILD  numeric components
#   AETHER_VERSION         "MAJOR.MINOR.PATCH.BUILD" (numeric, for project()/CPack)
#   AETHER_GIT_SHA         short commit hash or "unknown"
#   AETHER_VERSION_STRING  human-facing string, e.g. "0.1.0.42 (g04990dc)"
# ---------------------------------------------------------------------------

# --- Base MAJOR.MINOR.PATCH from the VERSION file --------------------------
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" _aether_base LIMIT_COUNT 1)
string(STRIP "${_aether_base}" _aether_base)
if(NOT _aether_base MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR
        "VERSION file must contain a MAJOR.MINOR.PATCH string, got: '${_aether_base}'")
endif()
set(AETHER_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(AETHER_VERSION_MINOR "${CMAKE_MATCH_2}")
set(AETHER_VERSION_PATCH "${CMAKE_MATCH_3}")

# --- Build number ----------------------------------------------------------
find_program(GIT_EXECUTABLE git)

if(NOT DEFINED AETHER_BUILD_NUMBER OR "${AETHER_BUILD_NUMBER}" STREQUAL "")
    set(AETHER_BUILD_NUMBER 0)
    if(GIT_EXECUTABLE)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _aether_count
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _aether_count_rc
            ERROR_QUIET)
        if(_aether_count_rc EQUAL 0 AND _aether_count MATCHES "^[0-9]+$")
            set(AETHER_BUILD_NUMBER "${_aether_count}")
        endif()
    endif()
endif()

# --- Short commit SHA ------------------------------------------------------
if(NOT DEFINED AETHER_GIT_SHA OR "${AETHER_GIT_SHA}" STREQUAL "")
    set(AETHER_GIT_SHA "unknown")
    if(GIT_EXECUTABLE)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _aether_sha
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _aether_sha_rc
            ERROR_QUIET)
        if(_aether_sha_rc EQUAL 0 AND NOT "${_aether_sha}" STREQUAL "")
            set(AETHER_GIT_SHA "${_aether_sha}")
        endif()
    endif()
endif()

# --- Composite strings -----------------------------------------------------
set(AETHER_VERSION
    "${AETHER_VERSION_MAJOR}.${AETHER_VERSION_MINOR}.${AETHER_VERSION_PATCH}.${AETHER_BUILD_NUMBER}")

if(AETHER_GIT_SHA STREQUAL "unknown")
    set(AETHER_VERSION_STRING "${AETHER_VERSION}")
else()
    set(AETHER_VERSION_STRING "${AETHER_VERSION} (g${AETHER_GIT_SHA})")
endif()

message(STATUS "AetherTFTP version: ${AETHER_VERSION_STRING}")
