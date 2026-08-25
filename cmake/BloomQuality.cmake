include_guard(GLOBAL)

option(BLOOM_ENABLE_CLANG_TIDY "Run clang-tidy while compiling Bloom-owned targets" OFF)
option(BLOOM_ENABLE_TEST_CLANG_TIDY "Also run clang-tidy while compiling test targets" OFF)
set(
    BLOOM_LLVM_TOOLCHAIN_MAJOR
    ""
    CACHE STRING
    "Required clang-format and clang-tidy major version; empty accepts the discovered tools"
)

# Bloom intentionally uses #pragma once; user-defined std::hash specializations are standard
# extension points; and enum storage is chosen for semantic/ABI needs rather than a blanket size
# heuristic. Keep those three checks disabled consistently instead of scattering NOLINT markers.
set(
    BLOOM_CLANG_TIDY_CHECKS
    "clang-analyzer-*,bugprone-*,-bugprone-easily-swappable-parameters,-bugprone-std-namespace-modification,performance-*,-performance-enum-size,portability-*,-portability-avoid-pragma-once,modernize-use-override,readability-container-size-empty,readability-duplicate-include,readability-qualified-auto"
    CACHE STRING
    "clang-tidy checks used when BLOOM_ENABLE_CLANG_TIDY is enabled"
)

function(bloom_require_llvm_tool_major executable tool_name)
    if(NOT BLOOM_LLVM_TOOLCHAIN_MAJOR)
        return()
    endif()
    if(NOT BLOOM_LLVM_TOOLCHAIN_MAJOR MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "BLOOM_LLVM_TOOLCHAIN_MAJOR must be a positive integer")
    endif()
    if(NOT executable)
        message(
            FATAL_ERROR
            "${tool_name} ${BLOOM_LLVM_TOOLCHAIN_MAJOR} was not found. "
            "Install that LLVM toolchain or set its Bloom executable cache entry."
        )
    endif()

    execute_process(
        COMMAND "${executable}" --version
        RESULT_VARIABLE bloom_tool_version_status
        OUTPUT_VARIABLE bloom_tool_version
        ERROR_VARIABLE bloom_tool_version_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(
        NOT bloom_tool_version_status EQUAL 0
        OR NOT bloom_tool_version MATCHES
            "version[ \t\r\n]+${BLOOM_LLVM_TOOLCHAIN_MAJOR}\\."
    )
        message(
            FATAL_ERROR
            "${tool_name} must use LLVM ${BLOOM_LLVM_TOOLCHAIN_MAJOR}; "
            "${executable} reported '${bloom_tool_version}${bloom_tool_version_error}'"
        )
    endif()
endfunction()

function(bloom_configure_quality_tools)
    set(bloom_clang_format_names clang-format)
    set(bloom_clang_tidy_names clang-tidy)
    if(BLOOM_LLVM_TOOLCHAIN_MAJOR)
        list(PREPEND bloom_clang_format_names "clang-format-${BLOOM_LLVM_TOOLCHAIN_MAJOR}")
        list(PREPEND bloom_clang_tidy_names "clang-tidy-${BLOOM_LLVM_TOOLCHAIN_MAJOR}")
    endif()

    find_program(
        BLOOM_CLANG_FORMAT_EXECUTABLE
        NAMES ${bloom_clang_format_names}
        DOC "Path to clang-format used by Bloom quality targets"
    )
    bloom_require_llvm_tool_major("${BLOOM_CLANG_FORMAT_EXECUTABLE}" "clang-format")

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
            NAMES ${bloom_clang_tidy_names}
            DOC "Path to clang-tidy used for Bloom-owned targets"
        )
        if(NOT BLOOM_CLANG_TIDY_EXECUTABLE)
            message(
                FATAL_ERROR
                "BLOOM_ENABLE_CLANG_TIDY is ON, but clang-tidy was not found. "
                "Set BLOOM_CLANG_TIDY_EXECUTABLE to its absolute path."
            )
        endif()
        bloom_require_llvm_tool_major("${BLOOM_CLANG_TIDY_EXECUTABLE}" "clang-tidy")

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
