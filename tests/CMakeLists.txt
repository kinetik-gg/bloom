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
        NAME bloom.quality.dependency_artifacts
        COMMAND
            ${bloom_python_command}
            "${PROJECT_SOURCE_DIR}/scripts/check_dependency_artifacts.py"
            --root
            "${PROJECT_SOURCE_DIR}"
        LABELS quality schema dependencies
        TIMEOUT 30
    )
    bloom_add_test(
        NAME bloom.quality.dependency_artifact_checker_self_tests
        COMMAND
            ${bloom_python_command}
            "${PROJECT_SOURCE_DIR}/tests/test_dependency_artifact_checks.py"
        LABELS quality schema dependencies unit
        TIMEOUT 30
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    )

    add_custom_target(
        bloom-dependency-artifact-check
        COMMAND
            ${bloom_python_command}
            "${PROJECT_SOURCE_DIR}/scripts/check_dependency_artifacts.py"
            --root
            "${PROJECT_SOURCE_DIR}"
        COMMENT "Checking dependency schema and synthetic canonical artifacts"
        VERBATIM
    )
    add_custom_target(
        bloom-quality-check
        DEPENDS
            bloom-format-check
            bloom-hygiene-check
            bloom-architecture-check
            bloom-project-schema-check
            bloom-dependency-artifact-check
    )
else()
    message(
        STATUS
        "Python 3.10+ was not found; schema quality script tests and targets are unavailable"
    )
endif()
