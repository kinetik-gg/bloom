include(BloomTesting)

add_library(
    bloom_dependency_artifact_checks
    STATIC
    dependency_artifact_canonical.cpp
    dependency_artifact_checks.cpp
    dependency_artifact_checks.hpp
    dependency_artifact_checks_internal.hpp
    dependency_artifact_schema.cpp
    dependency_lock_fixture.cpp
    dependency_prefix_fixture.cpp
)
target_link_libraries(bloom_dependency_artifact_checks PUBLIC bloom_quality_json PRIVATE bloom_core)
target_compile_features(bloom_dependency_artifact_checks PUBLIC cxx_std_20)
target_include_directories(bloom_dependency_artifact_checks PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
bloom_enable_warnings(bloom_dependency_artifact_checks)

add_executable(
    bloom_dependency_artifact_check
    dependency_artifact_checks_main.cpp
)
target_link_libraries(bloom_dependency_artifact_check PRIVATE bloom_dependency_artifact_checks)
bloom_enable_warnings(bloom_dependency_artifact_check)

if(BUILD_TESTING)
    add_executable(
        bloom_dependency_artifact_checks_test
        dependency_artifact_boundary_tests.cpp
        dependency_artifact_checks_tests.cpp
        dependency_artifact_checks_test_support.hpp
    )
    target_link_libraries(
        bloom_dependency_artifact_checks_test
        PRIVATE bloom_dependency_artifact_checks
    )
    bloom_enable_warnings(bloom_dependency_artifact_checks_test)

    bloom_add_test(
        NAME bloom.quality.dependency_artifacts
        COMMAND bloom_dependency_artifact_check --root "${PROJECT_SOURCE_DIR}"
        LABELS quality schema dependencies
        TIMEOUT 30
    )
    bloom_add_test(
        NAME bloom.quality.dependency_artifact_checker_self_tests
        COMMAND bloom_dependency_artifact_checks_test "${PROJECT_SOURCE_DIR}"
        LABELS quality schema dependencies unit
        TIMEOUT 30
    )
endif()

add_custom_target(
    bloom-dependency-artifact-check
    COMMAND
        $<TARGET_FILE:bloom_dependency_artifact_check>
        --root
        "${PROJECT_SOURCE_DIR}"
    DEPENDS bloom_dependency_artifact_check
    COMMENT "Checking dependency schema and synthetic canonical artifacts"
    VERBATIM
)
