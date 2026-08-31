bloom_find_dependency(yyjson)
bloom_find_dependency(libzip)

add_library(bloom_project STATIC)

target_sources(
    bloom_project
    PRIVATE
        canonical_base64.cpp
        canonical_decimal.cpp
        canonical_document.cpp
        canonical_json_string.cpp
        canonical_json_string_detail.hpp
        canonical_json_writer.cpp
        canonical_manifest.cpp
        document_decode.cpp
        document_decode_composition.cpp
        document_decode_internal.hpp
        document_migration.cpp
        document_reconstruct.cpp
        manifest_decode.cpp
        manifest_requirements.cpp
        open_archive.cpp
        project_io_memory.cpp
        project_io_memory_resource.cpp
        reopen_chain_internal.hpp
        round_trip_state.cpp
        save_archive.cpp
        save_archive_internal.hpp
        staged_copy.cpp
        staged_save.cpp
        strict_json_dom.cpp
        strict_json_preflight.cpp
        strict_json_preflight.hpp
        unknown_json_number.cpp
        zip_container.cpp
        zip_container_preflight.cpp
        zip_container_preflight.hpp
        zip_container_writer.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/project/canonical_base64.hpp
            include/bloom/project/canonical_decimal.hpp
            include/bloom/project/canonical_document.hpp
            include/bloom/project/canonical_json_string.hpp
            include/bloom/project/canonical_json_writer.hpp
            include/bloom/project/canonical_manifest.hpp
            include/bloom/project/document_decode.hpp
            include/bloom/project/document_migration.hpp
            include/bloom/project/document_reconstruct.hpp
            include/bloom/project/manifest_decode.hpp
            include/bloom/project/manifest_requirements.hpp
            include/bloom/project/open_archive.hpp
            include/bloom/project/project_io_memory.hpp
            include/bloom/project/project_io_memory_resource.hpp
            include/bloom/project/round_trip_state.hpp
            include/bloom/project/save_archive.hpp
            include/bloom/project/staged_copy.hpp
            include/bloom/project/staged_save.hpp
            include/bloom/project/strict_json_dom.hpp
            include/bloom/project/unknown_json_number.hpp
            include/bloom/project/zip_container.hpp
            include/bloom/project/zip_container_writer.hpp
)

target_compile_features(bloom_project PUBLIC cxx_std_20)
# bloom_platform is PUBLIC (not PRIVATE, despite the allowed dependency edge being unqualified):
# include/bloom/project/staged_save.hpp declares stageSaveArchive() taking a
# platform::StagedArtifactLease& in its public signature, so anything that includes that header
# needs bloom_platform's headers visible too. Every other bloom_project header stays
# platform-free; only this one new public header drives the PUBLIC requirement.
target_link_libraries(bloom_project PUBLIC bloom_core bloom_document bloom_platform)
target_link_libraries(bloom_project PRIVATE yyjson::yyjson libzip::zip)
bloom_enable_warnings(bloom_project)

if(BUILD_TESTING)
    include(BloomTesting)
    find_package(Threads REQUIRED)
    # Several test targets below link ZLIB::ZLIB for deflate/CRC fixtures. In qualified mode the
    # restricted libzip resolution already materializes that imported target via module-mode
    # FindZLIB, but in developer-system mode a host libzip package config may satisfy its zlib
    # dependency without ever creating it, and nothing else does -- the guard keeps both modes
    # configuring while never re-resolving a target the qualified path already pinned.
    if(NOT TARGET ZLIB::ZLIB)
        find_package(ZLIB REQUIRED)
    endif()
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

    add_executable(bloom_project_canonical_document_test tests/canonical_document_tests.cpp)
    target_link_libraries(bloom_project_canonical_document_test PRIVATE bloom_project)
    bloom_enable_warnings(bloom_project_canonical_document_test)
    bloom_add_test(
        NAME bloom.project.canonical_document
        COMMAND bloom_project_canonical_document_test
        LABELS unit project persistence document
    )

    add_executable(bloom_project_document_decode_test tests/document_decode_tests.cpp)
    target_link_libraries(bloom_project_document_decode_test PRIVATE bloom_project)
    bloom_enable_warnings(bloom_project_document_decode_test)
    bloom_add_test(
        NAME bloom.project.document_decode
        COMMAND bloom_project_document_decode_test
        LABELS unit project persistence document
    )

    add_executable(bloom_project_document_migration_test tests/document_migration_tests.cpp)
    target_link_libraries(bloom_project_document_migration_test PRIVATE bloom_project)
    bloom_enable_warnings(bloom_project_document_migration_test)
    bloom_add_test(
        NAME bloom.project.document_migration
        COMMAND bloom_project_document_migration_test
        LABELS unit project persistence document
    )

    add_executable(bloom_project_document_reconstruct_test tests/document_reconstruct_tests.cpp)
    target_link_libraries(bloom_project_document_reconstruct_test PRIVATE bloom_project)
    bloom_enable_warnings(bloom_project_document_reconstruct_test)
    bloom_add_test(
        NAME bloom.project.document_reconstruct
        COMMAND bloom_project_document_reconstruct_test
        LABELS unit project persistence document
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
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|GNU)$")
        # The allocation probe replaces global new and unsized delete. Keep every deallocation on
        # that measured path and avoid libstdc++ sized-delete builtin conflicts under clang-tidy.
        target_compile_options(
            bloom_project_strict_json_preflight_test
            PRIVATE -fno-sized-deallocation
        )
    endif()
    bloom_enable_warnings(bloom_project_strict_json_preflight_test)
    bloom_add_test(
        NAME bloom.project.strict_json_preflight
        COMMAND bloom_project_strict_json_preflight_test
        LABELS unit project persistence security
    )

    add_executable(bloom_project_strict_json_dom_test tests/strict_json_dom_tests.cpp)
    target_include_directories(
        bloom_project_strict_json_dom_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(bloom_project_strict_json_dom_test PRIVATE bloom_project)
    bloom_enable_warnings(bloom_project_strict_json_dom_test)
    bloom_add_test(
        NAME bloom.project.strict_json_dom
        COMMAND bloom_project_strict_json_dom_test
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

    add_executable(bloom_project_manifest_decode_test tests/manifest_decode_tests.cpp)
    target_link_libraries(bloom_project_manifest_decode_test PRIVATE bloom_project)
    bloom_enable_warnings(bloom_project_manifest_decode_test)
    bloom_add_test(
        NAME bloom.project.manifest_decode
        COMMAND bloom_project_manifest_decode_test
        LABELS unit project persistence manifest
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

    add_executable(bloom_project_unknown_json_number_test tests/unknown_json_number_tests.cpp)
    target_link_libraries(bloom_project_unknown_json_number_test PRIVATE bloom_project)
    bloom_enable_warnings(bloom_project_unknown_json_number_test)
    bloom_add_test(
        NAME bloom.project.unknown_json_number
        COMMAND bloom_project_unknown_json_number_test
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

    add_executable(
        bloom_project_zip_container_preflight_test
        tests/zip_container_preflight_tests.cpp
    )
    target_include_directories(
        bloom_project_zip_container_preflight_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(
        bloom_project_zip_container_preflight_test
        PRIVATE bloom_project ZLIB::ZLIB
    )
    bloom_enable_warnings(bloom_project_zip_container_preflight_test)
    bloom_add_test(
        NAME bloom.project.zip_container_preflight
        COMMAND bloom_project_zip_container_preflight_test
        LABELS unit project persistence security
    )

    add_executable(bloom_project_zip_container_test tests/zip_container_tests.cpp)
    target_link_libraries(
        bloom_project_zip_container_test
        PRIVATE bloom_project ZLIB::ZLIB
    )
    bloom_enable_warnings(bloom_project_zip_container_test)
    bloom_add_test(
        NAME bloom.project.zip_container
        COMMAND bloom_project_zip_container_test
        LABELS unit project persistence security
    )

    add_executable(bloom_project_zip_container_writer_test tests/zip_container_writer_tests.cpp)
    target_include_directories(
        bloom_project_zip_container_writer_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(
        bloom_project_zip_container_writer_test
        PRIVATE bloom_project ZLIB::ZLIB
    )
    bloom_enable_warnings(bloom_project_zip_container_writer_test)
    bloom_add_test(
        NAME bloom.project.zip_container_writer
        COMMAND bloom_project_zip_container_writer_test
        LABELS unit project persistence security
    )

    add_executable(bloom_project_save_archive_test tests/save_archive_tests.cpp)
    target_include_directories(
        bloom_project_save_archive_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(bloom_project_save_archive_test PRIVATE bloom_project ZLIB::ZLIB)
    bloom_enable_warnings(bloom_project_save_archive_test)
    bloom_add_test(
        NAME bloom.project.save_archive
        COMMAND bloom_project_save_archive_test
        LABELS unit project persistence security
    )

    add_executable(bloom_project_open_archive_test tests/open_archive_tests.cpp)
    target_include_directories(
        bloom_project_open_archive_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(bloom_project_open_archive_test PRIVATE bloom_project ZLIB::ZLIB)
    bloom_enable_warnings(bloom_project_open_archive_test)
    bloom_add_test(
        NAME bloom.project.open_archive
        COMMAND bloom_project_open_archive_test
        LABELS unit project persistence security
    )

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(bloom_project_staged_save_test tests/staged_save_tests.cpp)
        target_link_libraries(bloom_project_staged_save_test PRIVATE bloom_project)
        bloom_enable_warnings(bloom_project_staged_save_test)
        bloom_add_test(
            NAME bloom.project.staged_save
            COMMAND bloom_project_staged_save_test
            LABELS unit project persistence security
        )

        # Gated Linux-only exactly as bloom.project.staged_save is gated: this exercises a real
        # platform::StagedArtifactCoordinator over per-test temporary directories.
        add_executable(bloom_project_staged_copy_test tests/staged_copy_tests.cpp)
        target_link_libraries(bloom_project_staged_copy_test PRIVATE bloom_project)
        bloom_enable_warnings(bloom_project_staged_copy_test)
        bloom_add_test(
            NAME bloom.project.staged_copy
            COMMAND bloom_project_staged_copy_test
            LABELS unit project persistence security
        )
    endif()
endif()
