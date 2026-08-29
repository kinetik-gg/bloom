add_library(bloom_host STATIC)

target_sources(
    bloom_host
    PRIVATE
        project_session.cpp
        publication_coordinator.cpp
        save_publication.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/host/project_session.hpp
            include/bloom/host/publication_coordinator.hpp
            include/bloom/host/save_publication.hpp
)

target_compile_features(bloom_host PUBLIC cxx_std_20)
# bloom_project and bloom_platform are PUBLIC: include/bloom/host/save_publication.hpp declares
# SavePublicationRequest/SavePublicationResult carrying both modules' types (CanonicalManifestV1/
# CanonicalDocumentV1/SaveArchiveLimits/StagedSaveFailure, ArtifactOverwritePolicy/
# ArtifactTargetObservation/StagedArtifactCoordinator/StagedArtifactPublicationResult) in its
# public signatures, so anything including that header needs both modules' headers visible too.
target_link_libraries(bloom_host PUBLIC bloom_commands bloom_core bloom_document bloom_project
                                        bloom_platform)
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

    # Gated Linux-only exactly as bloom.project.staged_save is gated: this exercises a real
    # platform::StagedArtifactCoordinator over per-test temporary directories, and only the Linux
    # platform backend is implemented today (see src/platform/staged_artifact_unsupported.cpp).
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(bloom_host_save_publication_test tests/save_publication_tests.cpp)
        target_link_libraries(bloom_host_save_publication_test PRIVATE bloom_host)
        bloom_enable_warnings(bloom_host_save_publication_test)
        bloom_add_test(
            NAME bloom.host.save-publication
            COMMAND bloom_host_save_publication_test
            LABELS unit host persistence security
        )
    endif()
endif()
