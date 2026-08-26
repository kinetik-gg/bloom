# Bloom dependency-prefix consumption, per docs/architecture/dependency-intake.md.
#
# bloom_consume_dependency_prefix() validates and binds a dependency prefix produced by the
# superbuild under an explicit mode:
#
#   BLOOM_DEPENDENCY_MODE=qualified        validated production lock + restricted prefix search
#   BLOOM_DEPENDENCY_MODE=developer-system unrestricted local iteration, labeled Unqualified
#
# Staging note: no in-tree target consumes a dependency package yet. Until the first consuming
# target lands, the root build does not call this function and an absent mode is not an error;
# the contract's absence-is-an-error rule activates with the first consumer. The offline
# dependency-artifact checker (run by the test suite and CI) is the authority for full lock
# validation; the configure-time checks here enforce presence, identity-relevant toolchain
# agreement, and search restriction. Production prefix-manifest validation is deferred until the
# qualify phase writes one.

function(bloom_consume_dependency_prefix)
    if(NOT DEFINED BLOOM_DEPENDENCY_MODE)
        message(FATAL_ERROR
            "BLOOM_DEPENDENCY_MODE must be 'qualified' or 'developer-system' when a dependency "
            "prefix is consumed. There is no automatic fallback between modes.")
    endif()

    if(BLOOM_DEPENDENCY_MODE STREQUAL "developer-system")
        message(STATUS "Bloom dependencies: developer-system mode (Unqualified). This build "
            "cannot produce a release package, conformance result, or qualified capability.")
        set(BLOOM_DEPENDENCY_MODE_LABEL "Unqualified" PARENT_SCOPE)
        return()
    endif()

    if(NOT BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
        message(FATAL_ERROR
            "Unknown BLOOM_DEPENDENCY_MODE '${BLOOM_DEPENDENCY_MODE}'; expected 'qualified' or "
            "'developer-system'.")
    endif()

    if(NOT DEFINED BLOOM_DEPENDENCY_PREFIX OR NOT IS_DIRECTORY "${BLOOM_DEPENDENCY_PREFIX}")
        message(FATAL_ERROR
            "qualified mode requires BLOOM_DEPENDENCY_PREFIX to name the superbuild staging "
            "prefix; got '${BLOOM_DEPENDENCY_PREFIX}'.")
    endif()

    # The production lock lives at exactly this repository path; nothing inside an artifact may
    # redirect it, and the synthetic fixture tree is never a production source.
    set(bloom_lock "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../dependencies/dependencies.lock.json")
    cmake_path(NORMAL_PATH bloom_lock)
    if(NOT EXISTS "${bloom_lock}")
        message(FATAL_ERROR
            "qualified mode requires the reviewed production lock at "
            "dependencies/dependencies.lock.json; it is absent.")
    endif()

    file(READ "${bloom_lock}" bloom_lock_json)

    # Toolchain/ABI agreement, v1 enforced subset: compiler family and major version, C++
    # standard. The offline dependency-artifact checker enforces the complete lock contract in
    # the test phase; identity capture tightens with the production prefix manifest.
    string(JSON bloom_profile_count LENGTH "${bloom_lock_json}" "profiles")
    set(bloom_profile_matched FALSE)
    math(EXPR bloom_profile_last "${bloom_profile_count} - 1")
    foreach(bloom_profile_index RANGE 0 ${bloom_profile_last})
        string(JSON bloom_lock_family GET "${bloom_lock_json}"
            "profiles" ${bloom_profile_index} "consumerAbi" "compilerFamily")
        string(JSON bloom_lock_compiler_version GET "${bloom_lock_json}"
            "profiles" ${bloom_profile_index} "toolchain" "compiler" "version")
        string(JSON bloom_lock_standard GET "${bloom_lock_json}"
            "profiles" ${bloom_profile_index} "consumerAbi" "cxxStandard")

        set(bloom_actual_family "")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            set(bloom_actual_family "clang")
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set(bloom_actual_family "gcc")
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
            set(bloom_actual_family "apple-clang")
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            set(bloom_actual_family "msvc")
        endif()

        string(REGEX MATCH "^[0-9]+" bloom_lock_compiler_major "${bloom_lock_compiler_version}")
        string(REGEX MATCH "^[0-9]+" bloom_actual_compiler_major "${CMAKE_CXX_COMPILER_VERSION}")

        if(bloom_lock_family STREQUAL bloom_actual_family
           AND bloom_lock_compiler_major STREQUAL bloom_actual_compiler_major
           AND bloom_lock_standard EQUAL 20)
            set(bloom_profile_matched TRUE)
            break()
        endif()
    endforeach()
    if(NOT bloom_profile_matched)
        message(FATAL_ERROR
            "qualified mode: no locked profile agrees with this toolchain "
            "(${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}). The lock, not the host, "
            "is the authority; use developer-system mode for local iteration.")
    endif()

    # Restrict package resolution to the validated prefix. No host fallback, no registries, no
    # network-backed discovery.
    set(CMAKE_PREFIX_PATH "${BLOOM_DEPENDENCY_PREFIX}" PARENT_SCOPE)
    set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON PARENT_SCOPE)
    set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF PARENT_SCOPE)
    set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF PARENT_SCOPE)
    set(CMAKE_FIND_USE_CMAKE_SYSTEM_PATH OFF PARENT_SCOPE)
    set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OFF PARENT_SCOPE)
    set(BLOOM_DEPENDENCY_MODE_LABEL "Qualified-pending-manifest" PARENT_SCOPE)
    message(STATUS "Bloom dependencies: qualified mode against ${BLOOM_DEPENDENCY_PREFIX} "
        "(production lock present; full validation is enforced by the offline checker).")
endfunction()
