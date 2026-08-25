include(BloomTesting)

add_executable(bloom_ui_smoke_test
    ui_smoke.cpp
)

target_link_libraries(bloom_ui_smoke_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_smoke_test)

bloom_add_test(
    NAME bloom.ui.smoke
    COMMAND bloom_ui_smoke_test
    LABELS ui smoke
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

find_package(Python3 3.10 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    set(bloom_python_command "${Python3_EXECUTABLE}" -B)

    bloom_add_test(
        NAME bloom.quality.repository_hygiene
        COMMAND
            ${bloom_python_command}
            "${PROJECT_SOURCE_DIR}/scripts/check_repository_hygiene.py"
            --root
            "${PROJECT_SOURCE_DIR}"
        LABELS quality hygiene
        TIMEOUT 30
    )
    bloom_add_test(
        NAME bloom.quality.architecture_boundaries
        COMMAND
            ${bloom_python_command}
            "${PROJECT_SOURCE_DIR}/scripts/check_architecture_boundaries.py"
            --root
            "${PROJECT_SOURCE_DIR}"
        LABELS quality architecture
        TIMEOUT 30
    )
    bloom_add_test(
        NAME bloom.quality.checker_self_tests
        COMMAND
            ${bloom_python_command}
            "${PROJECT_SOURCE_DIR}/tests/test_repository_checks.py"
        LABELS quality unit
        TIMEOUT 30
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    )
    bloom_add_test(
        NAME bloom.quality.project_schemas
        COMMAND
            ${bloom_python_command}
            "${PROJECT_SOURCE_DIR}/scripts/check_project_schemas.py"
            --root
            "${PROJECT_SOURCE_DIR}"
        LABELS quality schema
        TIMEOUT 30
    )
    bloom_add_test(
        NAME bloom.quality.project_schema_checker_self_tests
        COMMAND
            ${bloom_python_command}
            "${PROJECT_SOURCE_DIR}/tests/test_project_schema_checks.py"
        LABELS quality schema unit
        TIMEOUT 30
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    )

    add_custom_target(
        bloom-hygiene-check
        COMMAND
            ${bloom_python_command}
            "${PROJECT_SOURCE_DIR}/scripts/check_repository_hygiene.py"
            --root
            "${PROJECT_SOURCE_DIR}"
        COMMENT "Checking repository hygiene"
        VERBATIM
    )
    add_custom_target(
        bloom-architecture-check
        COMMAND
            ${bloom_python_command}
            "${PROJECT_SOURCE_DIR}/scripts/check_architecture_boundaries.py"
            --root
            "${PROJECT_SOURCE_DIR}"
        COMMENT "Checking architecture boundaries"
        VERBATIM
    )
    add_custom_target(
        bloom-project-schema-check
        COMMAND
            ${bloom_python_command}
            "${PROJECT_SOURCE_DIR}/scripts/check_project_schemas.py"
            --root
            "${PROJECT_SOURCE_DIR}"
        COMMENT "Checking project schema artifacts"
        VERBATIM
    )
    add_custom_target(
        bloom-quality-check
        DEPENDS
            bloom-format-check
            bloom-hygiene-check
            bloom-architecture-check
            bloom-project-schema-check
    )
else()
    message(
        STATUS
        "Python 3.10+ was not found; repository quality script tests and targets are unavailable"
    )
endif()
