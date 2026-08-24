add_library(bloom_project STATIC)

target_sources(
    bloom_project
    PRIVATE
        canonical_base64.cpp
        canonical_decimal.cpp
        project_io_memory.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/project/canonical_base64.hpp
            include/bloom/project/canonical_decimal.hpp
            include/bloom/project/project_io_memory.hpp
)

target_compile_features(bloom_project PUBLIC cxx_std_20)
target_link_libraries(bloom_project PUBLIC bloom_core)
bloom_enable_warnings(bloom_project)

if(BUILD_TESTING)
    include(BloomTesting)
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
endif()
