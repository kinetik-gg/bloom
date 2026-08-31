add_library(bloom_output STATIC)

target_sources(
    bloom_output
    PRIVATE
        bound_output_analysis.cpp
        flat_exr_export_write.cpp
        flat_exr_output_adapter.cpp
        flat_exr_preset_contract.hpp
        flat_exr_reopen_verifier.cpp
        output_analysis.cpp
        output_analysis_analyzer.cpp
        output_analysis_attempt.cpp
        output_analysis_contract.cpp
        output_analysis_digest.cpp
        output_export_resource_ledger.cpp
        output_facet_descriptor.cpp
        output_analysis_numeric.cpp
        output_export_artifact_hash.hpp
        output_semantic_identity.cpp
        output_semantic_identity_numeric.cpp
        output_semantic_identity_ownership.cpp
        png_export_write.cpp
        png_output_adapter.cpp
        png_preset_contract.hpp
        png_reopen_verifier.cpp
        process_frame_semantic_identity.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/output/flat_exr_export_write.hpp
            include/bloom/output/flat_exr_output_adapter.hpp
            include/bloom/output/flat_exr_reopen_verifier.hpp
            include/bloom/output/output_analysis.hpp
            include/bloom/output/output_analysis_analyzer.hpp
            include/bloom/output/output_analysis_attempt.hpp
            include/bloom/output/output_analysis_digest.hpp
            include/bloom/output/output_export_resource_ledger.hpp
            include/bloom/output/output_export_stage.hpp
            include/bloom/output/output_facet_descriptor.hpp
            include/bloom/output/output_limits.hpp
            include/bloom/output/png_export_write.hpp
            include/bloom/output/png_output_adapter.hpp
            include/bloom/output/png_reopen_verifier.hpp
            include/bloom/output/process_frame_semantic_identity.hpp
)

target_compile_features(bloom_output PUBLIC cxx_std_20)
target_link_libraries(
    bloom_output
    # bloom_platform is PUBLIC: bloom/output/output_analysis_attempt.hpp's
    # OutputAnalysisAttemptTargetV1 carries bloom::platform::ArtifactTargetObservation/
    # ArtifactOverwritePolicy value types (the plain-data OUTCOME of a platform preflight call, not
    # a live StagedArtifactCoordinator/Target/Lease handle -- design decision 1's module split; see
    # output_analysis_attempt.hpp's own namespace comment). {"output", "platform"} was already
    # present in tools/quality/repository_checks/repository_checks.cpp's kAllowedModuleDependencies
    # allowlist before this task (no allowlist edit was needed).
    # bloom_color_ocio is PUBLIC (issue #111): bloom/output/output_analysis_attempt.hpp's
    # OutputAnalysisAttemptDisplayProductsV1 retains a
    # bloom::color::PreparedCpuDisplayProcessorHandle in its own public surface (the doc's
    # "Immutable Export Request" bullet: "the qualified PNG display-processor handle and identity
    # when applicable"), and png_export_write.cpp applies that handle through
    # bloom::runtime::CpuQualifiedDisplayPreparer. It was already reachable transitively through
    # bloom_runtime_evaluator's own PUBLIC bloom_color_ocio link; naming it here makes the direct
    # use explicit. No OpenColorIO type crosses either boundary (bloom_color_ocio links
    # OpenColorIO::OpenColorIO PRIVATE), and {"output", "color"} was already present in
    # tools/quality/repository_checks/repository_checks.cpp's kAllowedModuleDependencies allowlist.
    PUBLIC bloom_color bloom_color_ocio bloom_core bloom_platform bloom_render
           bloom_runtime_evaluator
)
bloom_enable_warnings(bloom_output)

# issue #99 (flat OpenEXR write adapter): the qualified prefix ships OpenEXR 3.4.15 directly (no
# OIIO); only this target links it, and only privately -- no OpenEXR/Imath type appears in any
# public bloom/output header (flat_exr_output_adapter.cpp/flat_exr_reopen_verifier.cpp are the
# only translation units that see real OpenEXR/Imath types), the same boundary discipline as
# bloom_color_ocio's OpenColorIO link. The bloom_find_dependency transitive sandbox the OCIO slice
# built (function-scoped CMAKE_PREFIX_PATH/NO_DEFAULT_PATH covering a package config's own nested
# find_dependency calls) carries OpenEXRConfig.cmake's find_dependency(Threads/Imath/libdeflate)
# chain with no changes needed here.
bloom_find_dependency(OpenEXR)
target_link_libraries(bloom_output PRIVATE OpenEXR::OpenEXR)

# issue #107 (PNG codec + strict chunk-profile verification): the PNG format layer is Bloom-owned
# over the qualified zlib (design decision 1) -- no OIIO, no libpng anywhere. bloom_find_dependency
# now resolves ZLIB in Config mode directly (issue #93 fixed the static-only prefix's
# ZLIBConfig.cmake so an unqualified find_package(ZLIB CONFIG) no longer hard-fails). That direct
# Config-mode resolution's own native CMake export names its imported target `ZLIB::ZLIBSTATIC`,
# not the conventional `ZLIB::ZLIB` src/project's tests link -- those get `ZLIB::ZLIB` only
# because it is transitively created by libzip's own nested Module-mode find_dependency(ZLIB)
# call (forced by this same function's ZLIB_ROOT/CMAKE_FIND_PACKAGE_PREFER_CONFIG handling while
# resolving libzip itself), never by a direct bloom_find_dependency(ZLIB) call like this one. Only
# png_output_adapter.cpp/png_reopen_verifier.cpp see the real zlib.h API, and never in a public
# bloom/output header.
bloom_find_dependency(ZLIB)
target_link_libraries(bloom_output PRIVATE ZLIB::ZLIBSTATIC)

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

    add_executable(
        bloom_output_flat_exr_output_adapter_test
        tests/flat_exr_output_adapter_tests.cpp
    )
    target_include_directories(
        bloom_output_flat_exr_output_adapter_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(
        bloom_output_flat_exr_output_adapter_test
        PRIVATE bloom_output OpenEXR::OpenEXR
    )
    bloom_enable_warnings(bloom_output_flat_exr_output_adapter_test)
    bloom_add_test(
        NAME bloom.output.flat_exr_output_adapter
        COMMAND bloom_output_flat_exr_output_adapter_test
        LABELS unit output render
    )

    add_executable(
        bloom_output_flat_exr_reopen_verifier_test
        tests/flat_exr_reopen_verifier_tests.cpp
    )
    target_include_directories(
        bloom_output_flat_exr_reopen_verifier_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(
        bloom_output_flat_exr_reopen_verifier_test
        PRIVATE bloom_output OpenEXR::OpenEXR
    )
    bloom_enable_warnings(bloom_output_flat_exr_reopen_verifier_test)
    bloom_add_test(
        NAME bloom.output.flat_exr_reopen_verifier
        COMMAND bloom_output_flat_exr_reopen_verifier_test
        LABELS unit output identity render
    )

    add_executable(
        bloom_output_semantic_identity_test
        tests/output_semantic_identity_tests.cpp
    )
    target_include_directories(
        bloom_output_semantic_identity_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(
        bloom_output_semantic_identity_test
        PRIVATE bloom_output
    )
    bloom_enable_warnings(bloom_output_semantic_identity_test)
    bloom_add_test(
        NAME bloom.output.semantic_identity
        COMMAND bloom_output_semantic_identity_test
        LABELS unit output identity serialization
    )

    # Task F2 (issue #101): the export-attempt/job orchestration library's own Qt-free pieces.
    add_executable(
        bloom_output_export_resource_ledger_test
        tests/output_export_resource_ledger_tests.cpp
    )
    target_link_libraries(
        bloom_output_export_resource_ledger_test
        PRIVATE bloom_output
    )
    bloom_enable_warnings(bloom_output_export_resource_ledger_test)
    bloom_add_test(
        NAME bloom.output.export_resource_ledger
        COMMAND bloom_output_export_resource_ledger_test
        LABELS unit output
    )

    add_executable(
        bloom_output_analysis_attempt_test
        tests/output_analysis_attempt_tests.cpp
    )
    target_include_directories(
        bloom_output_analysis_attempt_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(
        bloom_output_analysis_attempt_test
        PRIVATE bloom_output
    )
    bloom_enable_warnings(bloom_output_analysis_attempt_test)
    bloom_add_test(
        NAME bloom.output.analysis_attempt
        COMMAND bloom_output_analysis_attempt_test
        LABELS unit output identity render
    )

    add_executable(
        bloom_output_flat_exr_export_write_test
        tests/flat_exr_export_write_tests.cpp
    )
    target_include_directories(
        bloom_output_flat_exr_export_write_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(
        bloom_output_flat_exr_export_write_test
        PRIVATE bloom_output OpenEXR::OpenEXR
    )
    bloom_enable_warnings(bloom_output_flat_exr_export_write_test)
    bloom_add_test(
        NAME bloom.output.flat_exr_export_write
        COMMAND bloom_output_flat_exr_export_write_test
        LABELS unit output identity render
    )

    # issue #107 (PNG codec + strict chunk-profile verification).
    add_executable(
        bloom_output_png_output_adapter_test
        tests/png_output_adapter_tests.cpp
    )
    target_include_directories(
        bloom_output_png_output_adapter_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(
        bloom_output_png_output_adapter_test
        PRIVATE bloom_output ZLIB::ZLIBSTATIC
    )
    bloom_enable_warnings(bloom_output_png_output_adapter_test)
    bloom_add_test(
        NAME bloom.output.png_output_adapter
        COMMAND bloom_output_png_output_adapter_test
        LABELS unit output render
    )

    add_executable(
        bloom_output_png_reopen_verifier_test
        tests/png_reopen_verifier_tests.cpp
    )
    target_include_directories(
        bloom_output_png_reopen_verifier_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(
        bloom_output_png_reopen_verifier_test
        PRIVATE bloom_output ZLIB::ZLIBSTATIC
    )
    bloom_enable_warnings(bloom_output_png_reopen_verifier_test)
    bloom_add_test(
        NAME bloom.output.png_reopen_verifier
        COMMAND bloom_output_png_reopen_verifier_test
        LABELS unit output identity render
    )

    # issue #111 (PNG export through color preparation): the approved job's PNG
    # ColorPreparing/PreparingOutput/Writing/Verifying bridge over a real qualified Bloom Neutral
    # CPU display processor.
    add_executable(
        bloom_output_png_export_write_test
        tests/png_export_write_tests.cpp
    )
    target_include_directories(
        bloom_output_png_export_write_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(
        bloom_output_png_export_write_test
        PRIVATE bloom_output ZLIB::ZLIBSTATIC
    )
    bloom_enable_warnings(bloom_output_png_export_write_test)
    bloom_add_test(
        NAME bloom.output.png_export_write
        COMMAND bloom_output_png_export_write_test
        LABELS unit output color identity render
    )
endif()
