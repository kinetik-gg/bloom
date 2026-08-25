add_library(bloom_output STATIC)

target_sources(
    bloom_output
    PRIVATE
        process_frame_semantic_identity.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/output/process_frame_semantic_identity.hpp
)

target_compile_features(bloom_output PUBLIC cxx_std_20)
target_link_libraries(
    bloom_output
    PUBLIC bloom_core bloom_render bloom_runtime_evaluator
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
endif()
