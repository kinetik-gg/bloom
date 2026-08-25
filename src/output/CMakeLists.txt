add_library(bloom_output STATIC)

target_sources(
    bloom_output
    PRIVATE
        output_analysis.cpp
        output_analysis_analyzer.cpp
        output_analysis_contract.cpp
        output_analysis_digest.cpp
        output_facet_descriptor.cpp
        output_analysis_numeric.cpp
        process_frame_semantic_identity.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/output/output_analysis.hpp
            include/bloom/output/output_analysis_analyzer.hpp
            include/bloom/output/output_analysis_digest.hpp
            include/bloom/output/output_facet_descriptor.hpp
            include/bloom/output/output_limits.hpp
            include/bloom/output/process_frame_semantic_identity.hpp
)

target_compile_features(bloom_output PUBLIC cxx_std_20)
target_link_libraries(
    bloom_output
    PUBLIC bloom_color bloom_core bloom_render bloom_runtime_evaluator
)
bloom_enable_warnings(bloom_output)

if(BUILD_TESTING)
    include(BloomTesting)
    add_executable(
        bloom_output_process_frame_semantic_identity_test
        tests/process_frame_semantic_identity_tests.cpp
    )
    target_link_libraries(
        bloom_output_process_frame_semantic_identity_test
        PRIVATE bloom_output
    )
    bloom_enable_warnings(bloom_output_process_frame_semantic_identity_test)
    bloom_add_test(
        NAME bloom.output.process_frame_semantic_identity
        COMMAND bloom_output_process_frame_semantic_identity_test
        LABELS unit output identity render
    )

    add_executable(
        bloom_output_facet_descriptor_test
        tests/output_facet_descriptor_tests.cpp
    )
    target_link_libraries(
        bloom_output_facet_descriptor_test
        PRIVATE bloom_output
    )
    bloom_enable_warnings(bloom_output_facet_descriptor_test)
    bloom_add_test(
        NAME bloom.output.facet_descriptor
        COMMAND bloom_output_facet_descriptor_test
        LABELS unit output serialization
    )

    add_executable(
        bloom_output_analysis_test
        tests/output_analysis_tests.cpp
    )
    target_link_libraries(
        bloom_output_analysis_test
        PRIVATE bloom_output
    )
    bloom_enable_warnings(bloom_output_analysis_test)
    bloom_add_test(
        NAME bloom.output.analysis
        COMMAND bloom_output_analysis_test
        LABELS unit output serialization
    )

    add_executable(
        bloom_output_analysis_analyzer_test
        tests/output_analysis_analyzer_edge_tests.cpp
        tests/output_analysis_analyzer_tests.cpp
    )
    target_include_directories(
        bloom_output_analysis_analyzer_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(
        bloom_output_analysis_analyzer_test
        PRIVATE bloom_output
    )
    bloom_enable_warnings(bloom_output_analysis_analyzer_test)
    bloom_add_test(
        NAME bloom.output.analysis_analyzer
        COMMAND bloom_output_analysis_analyzer_test
        LABELS unit output identity serialization
    )

    add_executable(
        bloom_output_analysis_digest_test
        tests/output_analysis_digest_tests.cpp
    )
    target_link_libraries(
        bloom_output_analysis_digest_test
        PRIVATE bloom_output
    )
    bloom_enable_warnings(bloom_output_analysis_digest_test)
    bloom_add_test(
        NAME bloom.output.analysis_digest
        COMMAND bloom_output_analysis_digest_test
        LABELS unit output identity serialization
    )
endif()
