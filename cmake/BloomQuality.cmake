include_guard(GLOBAL)

option(BLOOM_ENABLE_CLANG_TIDY "Run clang-tidy while compiling Bloom-owned targets" OFF)
option(BLOOM_ENABLE_TEST_CLANG_TIDY "Also run clang-tidy while compiling test targets" OFF)

# Bloom intentionally uses #pragma once; user-defined std::hash specializations are standard
# extension points; and enum storage is chosen for semantic/ABI needs rather than a blanket size
# heuristic. Keep those three checks disabled consistently instead of scattering NOLINT markers.
set(
    BLOOM_CLANG_TIDY_CHECKS
    "clang-analyzer-*,bugprone-*,-bugprone-easily-swappable-parameters,-bugprone-std-namespace-modification,performance-*,-performance-enum-size,portability-*,-portability-avoid-pragma-once,modernize-use-override,readability-container-size-empty,readability-duplicate-include,readability-qualified-auto"
    CACHE STRING
    "clang-tidy checks used when BLOOM_ENABLE_CLANG_TIDY is enabled"
)

function(bloom_configure_quality_tools)
    find_program(
        BLOOM_CLANG_FORMAT_EXECUTABLE
        NAMES clang-format
        DOC "Path to clang-format used by Bloom quality targets"
    )

    add_custom_target(
        bloom-format-check
        COMMAND
            ${CMAKE_COMMAND}
            "-DBLOOM_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DBLOOM_CLANG_FORMAT_EXECUTABLE=${BLOOM_CLANG_FORMAT_EXECUTABLE}"
            -DBLOOM_FORMAT_MODE=check
            -P "${PROJECT_SOURCE_DIR}/cmake/RunClangFormat.cmake"
        COMMENT "Checking Bloom C++ formatting"
        VERBATIM
    )

    add_custom_target(
        bloom-format
        COMMAND
            ${CMAKE_COMMAND}
            "-DBLOOM_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DBLOOM_CLANG_FORMAT_EXECUTABLE=${BLOOM_CLANG_FORMAT_EXECUTABLE}"
            -DBLOOM_FORMAT_MODE=fix
            -P "${PROJECT_SOURCE_DIR}/cmake/RunClangFormat.cmake"
        COMMENT "Formatting Bloom C++ sources"
        VERBATIM
    )

    if(BLOOM_ENABLE_CLANG_TIDY)
        find_program(
            BLOOM_CLANG_TIDY_EXECUTABLE
            NAMES clang-tidy
            DOC "Path to clang-tidy used for Bloom-owned targets"
        )
        if(NOT BLOOM_CLANG_TIDY_EXECUTABLE)
            message(
                FATAL_ERROR
                "BLOOM_ENABLE_CLANG_TIDY is ON, but clang-tidy was not found. "
                "Set BLOOM_CLANG_TIDY_EXECUTABLE to its absolute path."
            )
        endif()

        set(
            BLOOM_CLANG_TIDY_COMMAND
            "${BLOOM_CLANG_TIDY_EXECUTABLE};--checks=${BLOOM_CLANG_TIDY_CHECKS};--warnings-as-errors=*;--header-filter=^${PROJECT_SOURCE_DIR}/(apps|src|tests|tools)/"
        )
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            # clang-tidy parses the real compiler command. Remove GCC-only diagnostics and
            # machine flags which Clang cannot parse while retaining the target's common flags.
            list(
                APPEND BLOOM_CLANG_TIDY_COMMAND
                --removed-arg=-mno-direct-extern-access
                --removed-arg=-Wcast-align=strict
                --removed-arg=-Wduplicated-branches
                --removed-arg=-Wduplicated-cond
                --removed-arg=-Wlogical-op
                --removed-arg=-Wuseless-cast
            )
        endif()
        set(
            BLOOM_CLANG_TIDY_COMMAND
            "${BLOOM_CLANG_TIDY_COMMAND}"
            CACHE INTERNAL
            "clang-tidy command for Bloom-owned targets"
            FORCE
        )
    else()
        unset(BLOOM_CLANG_TIDY_COMMAND CACHE)
    endif()
endfunction()

function(bloom_enable_clang_tidy target)
    if(BLOOM_ENABLE_CLANG_TIDY)
        if(NOT BLOOM_CLANG_TIDY_COMMAND)
            message(FATAL_ERROR "Bloom clang-tidy was enabled but not configured")
        endif()
        set_property(TARGET ${target} PROPERTY CXX_CLANG_TIDY "${BLOOM_CLANG_TIDY_COMMAND}")
    endif()
endfunction()
