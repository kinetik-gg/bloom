add_library(bloom_document STATIC)

target_sources(
    bloom_document
    PRIVATE
        document.cpp
        graph.cpp
        layer_stack.cpp
        new_project.cpp
        parameter.cpp
        project.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/document/composition_settings.hpp
            include/bloom/document/document.hpp
            include/bloom/document/graph.hpp
            include/bloom/document/ids.hpp
            include/bloom/document/layer_stack.hpp
            include/bloom/document/new_project.hpp
            include/bloom/document/parameter.hpp
            include/bloom/document/project.hpp
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
endif()
