add_library(bloom_host STATIC)

target_sources(
    bloom_host
    PRIVATE
        publication_coordinator.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/host/publication_coordinator.hpp
)

target_compile_features(bloom_host PUBLIC cxx_std_20)
target_link_libraries(bloom_host PUBLIC bloom_core)
bloom_enable_warnings(bloom_host)

if(BUILD_TESTING)
    include(BloomTesting)
    add_executable(
        bloom_host_publication_coordinator_test
        tests/publication_coordinator_tests.cpp
    )
    target_link_libraries(bloom_host_publication_coordinator_test PRIVATE bloom_host)
    bloom_enable_warnings(bloom_host_publication_coordinator_test)
    bloom_add_test(
        NAME bloom.host.publication-coordinator
        COMMAND bloom_host_publication_coordinator_test
        LABELS unit host project
    )
endif()
