add_library(bloom_host STATIC)

target_sources(
    bloom_host
    PRIVATE
        project_session.cpp
        publication_coordinator.cpp
        save_publication.cpp
        session_open.cpp
        session_save.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/host/bloom_neutral_profile.hpp
            include/bloom/host/project_session.hpp
            include/bloom/host/publication_coordinator.hpp
            include/bloom/host/save_publication.hpp
            include/bloom/host/session_open.hpp
            include/bloom/host/session_save.hpp
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

    # Offline drift guard for the immutable Bloom Neutral v1 built-in OCIO asset (issue #60):
    # locates the repository root via --root exactly as tools/quality/dependency_artifact_checks'
    # own bloom.quality.dependency_artifacts test does, rather than relying on an ambient working
    # directory.
    add_executable(bloom_host_bloom_neutral_profile_test tests/bloom_neutral_profile_tests.cpp)
    target_link_libraries(bloom_host_bloom_neutral_profile_test PRIVATE bloom_host)
    bloom_enable_warnings(bloom_host_bloom_neutral_profile_test)
    bloom_add_test(
        NAME bloom.host.bloom-neutral-profile
        COMMAND bloom_host_bloom_neutral_profile_test --root "${PROJECT_SOURCE_DIR}"
        LABELS unit host color quality
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

        # Gated Linux-only exactly as bloom.host.save-publication is gated: saveProjectSession()
        # drives the same real platform::StagedArtifactCoordinator.
        add_executable(bloom_host_session_save_test tests/session_save_tests.cpp)
        target_link_libraries(bloom_host_session_save_test PRIVATE bloom_host)
        bloom_enable_warnings(bloom_host_session_save_test)
        bloom_add_test(
            NAME bloom.host.session-save
            COMMAND bloom_host_session_save_test
            LABELS unit host persistence security
        )

        # Gated Linux-only exactly as bloom.host.session-save is gated: openSessionArchive()'s own
        # full-cycle tests drive saveProjectSession() over the same real
        # platform::StagedArtifactCoordinator to produce a published file to reopen.
        #
        # Links ZLIB::ZLIB directly for its own trimmed hand-rolled ZIP-writer fixture (a
        # preserved-read-only archive builder) -- mirroring src/project/CMakeLists.txt's own test
        # targets that link ZLIB::ZLIB for the identical reason. src/project/CMakeLists.txt already
        # ran (project is add_subdirectory'd before host) and defines this target when
        # BUILD_TESTING is set, exactly as it is here; guarded anyway for robustness.
        if(NOT TARGET ZLIB::ZLIB)
            find_package(ZLIB REQUIRED)
        endif()
        add_executable(bloom_host_session_open_test tests/session_open_tests.cpp)
        target_link_libraries(bloom_host_session_open_test PRIVATE bloom_host ZLIB::ZLIB)
        bloom_enable_warnings(bloom_host_session_open_test)
        bloom_add_test(
            NAME bloom.host.session-open
            COMMAND bloom_host_session_open_test
            LABELS unit host persistence security
        )
    endif()
endif()
