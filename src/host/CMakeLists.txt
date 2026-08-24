add_library(bloom_host STATIC)

target_sources(
    bloom_host
    PRIVATE
        project_session.cpp
        publication_coordinator.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/host/project_session.hpp
            include/bloom/host/publication_coordinator.hpp
)

target_compile_features(bloom_host PUBLIC cxx_std_20)
target_link_libraries(bloom_host PUBLIC bloom_commands bloom_core bloom_document)
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

    add_executable(bloom_host_project_session_test tests/project_session_tests.cpp)
    target_link_libraries(bloom_host_project_session_test PRIVATE bloom_host)
    bloom_enable_warnings(bloom_host_project_session_test)
    bloom_add_test(
        NAME bloom.host.project-session
        COMMAND bloom_host_project_session_test
        LABELS unit host project commands
    )
endif()
