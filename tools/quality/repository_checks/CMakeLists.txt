include(BloomTesting)

add_library(
    bloom_repository_checks
    STATIC
    repository_checks.cpp
    repository_checks.hpp
)
target_include_directories(bloom_repository_checks PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(bloom_repository_checks PUBLIC cxx_std_20)
bloom_enable_warnings(bloom_repository_checks)

add_library(bloom_repository_checker_main STATIC checker_main.cpp checker_main.hpp)
target_link_libraries(bloom_repository_checker_main PUBLIC bloom_repository_checks)
bloom_enable_warnings(bloom_repository_checker_main)

add_executable(bloom_repository_hygiene_check repository_hygiene_main.cpp)
target_link_libraries(bloom_repository_hygiene_check PRIVATE bloom_repository_checker_main)
bloom_enable_warnings(bloom_repository_hygiene_check)

add_executable(bloom_architecture_boundaries_check architecture_boundaries_main.cpp)
target_link_libraries(bloom_architecture_boundaries_check PRIVATE bloom_repository_checker_main)
bloom_enable_warnings(bloom_architecture_boundaries_check)

add_custom_target(
    bloom-hygiene-check
    COMMAND
        ${CMAKE_COMMAND}
        "-DBLOOM_REPOSITORY_CHECKER=$<TARGET_FILE:bloom_repository_hygiene_check>"
        "-DBLOOM_REPOSITORY_ROOT=${PROJECT_SOURCE_DIR}"
        "-DBLOOM_REPOSITORY_FILE_MANIFEST=${PROJECT_BINARY_DIR}/quality/repository-hygiene.files"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/RunRepositoryCheck.cmake"
    DEPENDS bloom_repository_hygiene_check
    COMMENT "Checking repository hygiene"
    VERBATIM
)
add_custom_target(
    bloom-architecture-check
    COMMAND
        ${CMAKE_COMMAND}
        "-DBLOOM_REPOSITORY_CHECKER=$<TARGET_FILE:bloom_architecture_boundaries_check>"
        "-DBLOOM_REPOSITORY_ROOT=${PROJECT_SOURCE_DIR}"
        "-DBLOOM_REPOSITORY_FILE_MANIFEST=${PROJECT_BINARY_DIR}/quality/architecture-boundaries.files"
        -P
        "${CMAKE_CURRENT_SOURCE_DIR}/RunRepositoryCheck.cmake"
    DEPENDS bloom_architecture_boundaries_check
    COMMENT "Checking architecture boundaries"
    VERBATIM
)

if(BUILD_TESTING)
    add_executable(
        bloom_repository_checks_test
        tests/repository_checks_tests.cpp
    )
    target_link_libraries(bloom_repository_checks_test PRIVATE bloom_repository_checks)
    bloom_enable_warnings(bloom_repository_checks_test)

    bloom_add_test(
        NAME bloom.quality.repository_hygiene
        COMMAND
            ${CMAKE_COMMAND}
            "-DBLOOM_REPOSITORY_CHECKER=$<TARGET_FILE:bloom_repository_hygiene_check>"
            "-DBLOOM_REPOSITORY_ROOT=${PROJECT_SOURCE_DIR}"
            "-DBLOOM_REPOSITORY_FILE_MANIFEST=${PROJECT_BINARY_DIR}/quality/repository-hygiene-test.files"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/RunRepositoryCheck.cmake"
        LABELS quality hygiene
        TIMEOUT 30
    )
    bloom_add_test(
        NAME bloom.quality.architecture_boundaries
        COMMAND
            ${CMAKE_COMMAND}
            "-DBLOOM_REPOSITORY_CHECKER=$<TARGET_FILE:bloom_architecture_boundaries_check>"
            "-DBLOOM_REPOSITORY_ROOT=${PROJECT_SOURCE_DIR}"
            "-DBLOOM_REPOSITORY_FILE_MANIFEST=${PROJECT_BINARY_DIR}/quality/architecture-boundaries-test.files"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/RunRepositoryCheck.cmake"
        LABELS quality architecture
        TIMEOUT 30
    )
    bloom_add_test(
        NAME bloom.quality.checker_self_tests
        COMMAND bloom_repository_checks_test
        LABELS quality unit
        TIMEOUT 30
    )
endif()
