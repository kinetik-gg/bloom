if(NOT DEFINED BLOOM_SOURCE_DIR OR BLOOM_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "BLOOM_SOURCE_DIR is required")
endif()

if(
    NOT DEFINED BLOOM_CLANG_FORMAT_EXECUTABLE
    OR BLOOM_CLANG_FORMAT_EXECUTABLE STREQUAL ""
    OR NOT EXISTS "${BLOOM_CLANG_FORMAT_EXECUTABLE}"
)
    message(
        FATAL_ERROR
        "clang-format was not found. Install it or configure "
        "BLOOM_CLANG_FORMAT_EXECUTABLE, then run bloom-format-check again."
    )
endif()

if(NOT BLOOM_FORMAT_MODE MATCHES "^(check|fix)$")
    message(FATAL_ERROR "BLOOM_FORMAT_MODE must be check or fix")
endif()

file(
    GLOB_RECURSE bloom_format_sources
    LIST_DIRECTORIES FALSE
    "${BLOOM_SOURCE_DIR}/apps/*.c"
    "${BLOOM_SOURCE_DIR}/apps/*.cc"
    "${BLOOM_SOURCE_DIR}/apps/*.cpp"
    "${BLOOM_SOURCE_DIR}/apps/*.cxx"
    "${BLOOM_SOURCE_DIR}/apps/*.h"
    "${BLOOM_SOURCE_DIR}/apps/*.hh"
    "${BLOOM_SOURCE_DIR}/apps/*.hpp"
    "${BLOOM_SOURCE_DIR}/apps/*.hxx"
    "${BLOOM_SOURCE_DIR}/src/*.c"
    "${BLOOM_SOURCE_DIR}/src/*.cc"
    "${BLOOM_SOURCE_DIR}/src/*.cpp"
    "${BLOOM_SOURCE_DIR}/src/*.cxx"
    "${BLOOM_SOURCE_DIR}/src/*.h"
    "${BLOOM_SOURCE_DIR}/src/*.hh"
    "${BLOOM_SOURCE_DIR}/src/*.hpp"
    "${BLOOM_SOURCE_DIR}/src/*.hxx"
    "${BLOOM_SOURCE_DIR}/tests/*.c"
    "${BLOOM_SOURCE_DIR}/tests/*.cc"
    "${BLOOM_SOURCE_DIR}/tests/*.cpp"
    "${BLOOM_SOURCE_DIR}/tests/*.cxx"
    "${BLOOM_SOURCE_DIR}/tests/*.h"
    "${BLOOM_SOURCE_DIR}/tests/*.hh"
    "${BLOOM_SOURCE_DIR}/tests/*.hpp"
    "${BLOOM_SOURCE_DIR}/tests/*.hxx"
)
list(SORT bloom_format_sources)

if(NOT bloom_format_sources)
    message(FATAL_ERROR "No Bloom C++ sources were found to format")
endif()

set(bloom_format_failures "")
foreach(source IN LISTS bloom_format_sources)
    if(BLOOM_FORMAT_MODE STREQUAL "check")
        execute_process(
            COMMAND
                "${BLOOM_CLANG_FORMAT_EXECUTABLE}"
                --style=file
                --fallback-style=none
                --dry-run
                --Werror
                "${source}"
            RESULT_VARIABLE result
        )
    else()
        execute_process(
            COMMAND
                "${BLOOM_CLANG_FORMAT_EXECUTABLE}"
                --style=file
                --fallback-style=none
                -i
                "${source}"
            RESULT_VARIABLE result
        )
    endif()

    if(NOT result EQUAL 0)
        list(APPEND bloom_format_failures "${source}")
    endif()
endforeach()

if(bloom_format_failures)
    list(JOIN bloom_format_failures "\n  " formatted_failures)
    if(BLOOM_FORMAT_MODE STREQUAL "check")
        message(
            FATAL_ERROR
            "clang-format rejected these files:\n  ${formatted_failures}\n"
            "Run: cmake --build --preset dev --target bloom-format"
        )
    endif()
    message(FATAL_ERROR "clang-format failed for:\n  ${formatted_failures}")
endif()

list(LENGTH bloom_format_sources bloom_format_source_count)
message(STATUS "clang-format ${BLOOM_FORMAT_MODE} passed for ${bloom_format_source_count} files")
