add_library(bloom_document STATIC)

target_sources(
    bloom_document
    PRIVATE
        animation.cpp
        document.cpp
        extension_records.cpp
        graph.cpp
        layer_stack.cpp
        new_project.cpp
        parameter.cpp
        persisted_text.cpp
        project.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/document/animation.hpp
            include/bloom/document/composition_settings.hpp
            include/bloom/document/document.hpp
            include/bloom/document/extension_records.hpp
            include/bloom/document/graph.hpp
            include/bloom/document/ids.hpp
            include/bloom/document/layer_stack.hpp
            include/bloom/document/new_project.hpp
            include/bloom/document/parameter.hpp
            include/bloom/document/persisted_text.hpp
            include/bloom/document/project.hpp
            include/bloom/document/schema_version.hpp
            include/bloom/document/validation.hpp
)

target_compile_features(bloom_document PUBLIC cxx_std_20)
target_link_libraries(bloom_document PUBLIC bloom_core)
bloom_enable_warnings(bloom_document)

if(BUILD_TESTING)
    include(BloomTesting)

    add_executable(bloom_document_model_test tests/document_model_tests.cpp)
    target_link_libraries(bloom_document_model_test PRIVATE bloom_document)
    bloom_enable_warnings(bloom_document_model_test)
    bloom_add_test(NAME bloom.document.model COMMAND bloom_document_model_test LABELS unit)

    add_executable(bloom_document_publication_test tests/document_publication_tests.cpp)
    target_link_libraries(bloom_document_publication_test PRIVATE bloom_document)
    bloom_enable_warnings(bloom_document_publication_test)
    bloom_add_test(
        NAME bloom.document.publication
        COMMAND bloom_document_publication_test
        LABELS unit
    )

    add_executable(bloom_document_animation_model_test tests/animation_model_tests.cpp)
    target_link_libraries(bloom_document_animation_model_test PRIVATE bloom_document)
    bloom_enable_warnings(bloom_document_animation_model_test)
    bloom_add_test(
        NAME bloom.document.animation-model
        COMMAND bloom_document_animation_model_test
        LABELS unit animation
    )

    add_executable(bloom_document_extension_records_test tests/extension_records_tests.cpp)
    target_link_libraries(bloom_document_extension_records_test PRIVATE bloom_document)
    bloom_enable_warnings(bloom_document_extension_records_test)
    bloom_add_test(
        NAME bloom.document.extension-records
        COMMAND bloom_document_extension_records_test
        LABELS unit persistence
    )

    add_executable(bloom_document_persisted_text_test tests/persisted_text_tests.cpp)
    target_link_libraries(bloom_document_persisted_text_test PRIVATE bloom_document)
    bloom_enable_warnings(bloom_document_persisted_text_test)
    bloom_add_test(
        NAME bloom.document.persisted-text
        COMMAND bloom_document_persisted_text_test
        LABELS unit persistence text
    )
endif()
