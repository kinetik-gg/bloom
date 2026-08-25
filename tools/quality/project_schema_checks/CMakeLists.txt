add_library(
    bloom_project_schema_checks
    STATIC
    document_schema_checks.cpp
    document_schema_graph_checks.cpp
    manifest_schema_checks.cpp
    schema_check_helpers.cpp
    schema_check_helpers.hpp
    schema_checks.hpp
)
target_include_directories(bloom_project_schema_checks PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(bloom_project_schema_checks PUBLIC bloom_quality_json)
bloom_enable_warnings(bloom_project_schema_checks)

add_executable(bloom_project_schema_check project_schema_main.cpp)
target_link_libraries(bloom_project_schema_check PRIVATE bloom_project_schema_checks)
bloom_enable_warnings(bloom_project_schema_check)

if(BUILD_TESTING)
    include(BloomTesting)
    add_executable(bloom_project_schema_checks_test project_schema_checks_tests.cpp)
    target_link_libraries(bloom_project_schema_checks_test PRIVATE bloom_project_schema_checks)
    bloom_enable_warnings(bloom_project_schema_checks_test)

    bloom_add_test(
        NAME bloom.quality.project_schemas
        COMMAND bloom_project_schema_check --root "${PROJECT_SOURCE_DIR}"
        LABELS quality schema
        TIMEOUT 30
    )
    bloom_add_test(
        NAME bloom.quality.project_schema_checker_self_tests
        COMMAND bloom_project_schema_checks_test --root "${PROJECT_SOURCE_DIR}"
        LABELS quality schema unit
        TIMEOUT 30
    )
endif()

add_custom_target(
    bloom-project-schema-check
    COMMAND $<TARGET_FILE:bloom_project_schema_check> --root "${PROJECT_SOURCE_DIR}"
    DEPENDS bloom_project_schema_check
    COMMENT "Checking project schema artifacts"
    VERBATIM
)
