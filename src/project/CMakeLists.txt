add_library(bloom_project STATIC)

target_sources(
    bloom_project
    PRIVATE
        canonical_base64.cpp
        canonical_decimal.cpp
        canonical_json_string.cpp
        canonical_json_string_detail.hpp
        canonical_json_writer.cpp
        canonical_manifest.cpp
        manifest_requirements.cpp
        project_io_memory.cpp
        project_io_memory_resource.cpp
        strict_json_preflight.cpp
        strict_json_preflight.hpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/project/canonical_base64.hpp
            include/bloom/project/canonical_decimal.hpp
            include/bloom/project/canonical_json_string.hpp
            include/bloom/project/canonical_json_writer.hpp
            include/bloom/project/canonical_manifest.hpp
            include/bloom/project/manifest_requirements.hpp
            include/bloom/project/project_io_memory.hpp
            include/bloom/project/project_io_memory_resource.hpp
)

target_compile_features(bloom_project PUBLIC cxx_std_20)
target_link_libraries(bloom_project PUBLIC bloom_core bloom_document)
bloom_enable_warnings(bloom_project)

if(BUILD_TESTING)
    include(BloomTesting)
    find_package(Threads REQUIRED)
    add_executable(bloom_project_canonical_json_string_test tests/canonical_json_string_tests.cpp)
    target_include_directories(
        bloom_project_canonical_json_string_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(bloom_project_canonical_json_string_test PRIVATE bloom_project)
    bloom_enable_warnings(bloom_project_canonical_json_string_test)
    bloom_add_test(
        NAME bloom.project.canonical_json_string
        COMMAND bloom_project_canonical_json_string_test
        LABELS unit project persistence text
    )

    add_executable(bloom_project_canonical_manifest_test tests/canonical_manifest_tests.cpp)
    target_link_libraries(bloom_project_canonical_manifest_test PRIVATE bloom_project)
    bloom_enable_warnings(bloom_project_canonical_manifest_test)
    bloom_add_test(
        NAME bloom.project.canonical_manifest
        COMMAND bloom_project_canonical_manifest_test
        LABELS unit project persistence manifest
    )

    add_executable(bloom_project_canonical_json_writer_test tests/canonical_json_writer_tests.cpp)
    target_link_libraries(bloom_project_canonical_json_writer_test PRIVATE bloom_project)
    bloom_enable_warnings(bloom_project_canonical_json_writer_test)
    bloom_add_test(
        NAME bloom.project.canonical_json_writer
        COMMAND bloom_project_canonical_json_writer_test
        LABELS unit project persistence text
    )

    add_executable(
        bloom_project_strict_json_preflight_test
        tests/strict_json_preflight_tests.cpp
    )
    target_include_directories(
        bloom_project_strict_json_preflight_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(bloom_project_strict_json_preflight_test PRIVATE bloom_project)
    bloom_enable_warnings(bloom_project_strict_json_preflight_test)
    bloom_add_test(
        NAME bloom.project.strict_json_preflight
        COMMAND bloom_project_strict_json_preflight_test
        LABELS unit project persistence security
    )

    add_executable(bloom_project_manifest_requirements_test tests/manifest_requirements_tests.cpp)
    target_link_libraries(bloom_project_manifest_requirements_test PRIVATE bloom_project)
    bloom_enable_warnings(bloom_project_manifest_requirements_test)
    bloom_add_test(
        NAME bloom.project.manifest_requirements
        COMMAND bloom_project_manifest_requirements_test
        LABELS unit project persistence document
    )

    add_executable(bloom_project_canonical_base64_test tests/canonical_base64_tests.cpp)
    target_link_libraries(bloom_project_canonical_base64_test PRIVATE bloom_project)
    bloom_enable_warnings(bloom_project_canonical_base64_test)
    bloom_add_test(
        NAME bloom.project.canonical_base64
        COMMAND bloom_project_canonical_base64_test
        LABELS unit project persistence
    )

    add_executable(bloom_project_canonical_decimal_test tests/canonical_decimal_tests.cpp)
    target_link_libraries(bloom_project_canonical_decimal_test PRIVATE bloom_project)
    bloom_enable_warnings(bloom_project_canonical_decimal_test)
    bloom_add_test(
        NAME bloom.project.canonical_decimal
        COMMAND bloom_project_canonical_decimal_test
        LABELS unit project persistence
    )

    add_executable(bloom_project_io_memory_test tests/project_io_memory_tests.cpp)
    target_link_libraries(bloom_project_io_memory_test PRIVATE bloom_project)
    bloom_enable_warnings(bloom_project_io_memory_test)
    bloom_add_test(
        NAME bloom.project.io_memory
        COMMAND bloom_project_io_memory_test
        LABELS unit project
    )

    add_executable(
        bloom_project_io_memory_resource_test
        tests/project_io_memory_resource_tests.cpp
    )
    target_link_libraries(
        bloom_project_io_memory_resource_test
        PRIVATE bloom_project Threads::Threads
    )
    bloom_enable_warnings(bloom_project_io_memory_resource_test)
    bloom_add_test(
        NAME bloom.project.io_memory_resource
        COMMAND bloom_project_io_memory_resource_test
        LABELS unit project persistence memory
    )
endif()
