include(BloomTesting)

add_executable(bloom_render_image_test render_image_test.cpp)
target_link_libraries(bloom_render_image_test PRIVATE bloom_render)
bloom_enable_warnings(bloom_render_image_test)
bloom_enable_strict_floating_point(bloom_render_image_test)
bloom_add_test(NAME bloom.render.image COMMAND bloom_render_image_test LABELS unit)

add_executable(bloom_render_cpu_image_primitives_test cpu_image_primitives_test.cpp)
target_link_libraries(bloom_render_cpu_image_primitives_test PRIVATE bloom_render)
bloom_enable_warnings(bloom_render_cpu_image_primitives_test)
bloom_enable_strict_floating_point(bloom_render_cpu_image_primitives_test)
bloom_add_test(
    NAME bloom.render.cpu_image_primitives
    COMMAND bloom_render_cpu_image_primitives_test
    LABELS unit primitives
)

add_executable(bloom_render_text_raster_test text_raster_test.cpp)
target_link_libraries(bloom_render_text_raster_test PRIVATE bloom_render)
bloom_enable_warnings(bloom_render_text_raster_test)
bloom_enable_strict_floating_point(bloom_render_text_raster_test)
bloom_add_test(
    NAME bloom.render.text_raster
    COMMAND bloom_render_text_raster_test
    LABELS unit primitives text
)

add_executable(bloom_render_path_raster_test path_raster_test.cpp)
target_link_libraries(bloom_render_path_raster_test PRIVATE bloom_render)
bloom_enable_warnings(bloom_render_path_raster_test)
bloom_enable_strict_floating_point(bloom_render_path_raster_test)
bloom_add_test(NAME bloom.render.path_raster COMMAND bloom_render_path_raster_test LABELS unit primitives)

# CPU-only exactness test for PathRasterCoverageGeometry: reconstructs the uint8 mask from the
# integer sample spans with exactly the GPU shader's arithmetic and asserts byte-identity to
# coverageRow over every shape, rule, stroke, clip, transform, proxy and adversarial case. No device.
add_executable(bloom_render_gpu_path_coverage_geometry_test gpu_path_coverage_geometry_tests.cpp)
target_link_libraries(bloom_render_gpu_path_coverage_geometry_test PRIVATE bloom_render)
bloom_enable_warnings(bloom_render_gpu_path_coverage_geometry_test)
bloom_enable_strict_floating_point(bloom_render_gpu_path_coverage_geometry_test)
bloom_add_test(
    NAME bloom.render.gpu_path_coverage_geometry
    COMMAND bloom_render_gpu_path_coverage_geometry_test
    LABELS unit gpu render
)

# The GPU device test compiles and passes identically against the Vulkan bootstrap or the CPU stub:
# the real probe runs only when a compatible device is present, and a hardware-free image prints an
# explicit skip while the forced-missing-loader checks still exercise the portable API.
add_executable(bloom_render_gpu_device_test gpu_device_tests.cpp)
target_link_libraries(bloom_render_gpu_device_test PRIVATE bloom_render)
if(BLOOM_RENDER_VULKAN_AVAILABLE)
    target_compile_definitions(bloom_render_gpu_device_test
        PRIVATE BLOOM_RENDER_TEST_VULKAN_BACKEND=1)
endif()
bloom_enable_warnings(bloom_render_gpu_device_test)
bloom_enable_strict_floating_point(bloom_render_gpu_device_test)
bloom_add_test(
    NAME bloom.render.gpu_device
    COMMAND bloom_render_gpu_device_test
    LABELS unit gpu
)

# The GPU Neutral V1 display and GPU-resident Neutral V1 display tests use the independent OCIO CPU
# oracle, which lives in src/color, so their sources are owned by src/runtime/tests (runtime may
# depend on both render and color). Their registered test names and labels are unchanged.

# SolidV1 resident-image test. Compiles unconditionally and depends only on runtime create()
# availability, so the same target runs against the CPU stub. It hashes the embedded SPIR-V array
# against the pinned digest, so a changed array word fails even if comments are intact.
add_executable(bloom_render_gpu_solid_test gpu_solid_tests.cpp)
target_link_libraries(bloom_render_gpu_solid_test PRIVATE bloom_render)
target_include_directories(bloom_render_gpu_solid_test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan")
target_compile_definitions(bloom_render_gpu_solid_test
    PRIVATE "BLOOM_SOLID_SPV_SHA256=\"2fae4aef67267e98a53034448639bfb00a69568ec3be1d8ed1a1bc60b31a0318\"")
bloom_enable_warnings(bloom_render_gpu_solid_test)
bloom_enable_strict_floating_point(bloom_render_gpu_solid_test)
bloom_add_test(
    NAME bloom.render.gpu_solid
    COMMAND bloom_render_gpu_solid_test
    LABELS unit gpu render
)

# CoveredSolidV1 resident-image test (the CPU vector-coverage path). Compiles unconditionally and
# depends only on runtime create() availability, so the same target runs against the CPU stub. It
# hashes the embedded SPIR-V array against the pinned digest, so a changed array word fails.
add_executable(bloom_render_gpu_solid_covered_test gpu_solid_covered_tests.cpp)
target_link_libraries(bloom_render_gpu_solid_covered_test PRIVATE bloom_render)
target_include_directories(bloom_render_gpu_solid_covered_test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan")
target_compile_definitions(bloom_render_gpu_solid_covered_test
    PRIVATE "BLOOM_SOLID_COVERED_SPV_SHA256=\"3d54bcb0b9394b381df9f271cdfd05af4cb2f80620ba142227fec53ba26e1a9b\"")
bloom_enable_warnings(bloom_render_gpu_solid_covered_test)
bloom_enable_strict_floating_point(bloom_render_gpu_solid_covered_test)
bloom_add_test(
    NAME bloom.render.gpu_solid_covered
    COMMAND bloom_render_gpu_solid_covered_test
    LABELS unit gpu render
)

# GpuPathCoverage native producer test. Builds the immutable scanline geometry from the real CPU
# PathRaster and asserts the resident mask readback is byte-identical to the unchanged
# coverageRow for the shape/transform/stroke/fill-rule/clip/adversarial matrix. It also proves one
# resident coverage -> GpuSolid covered fill -> native output chain. Compiles against the CPU stub
# as well; a hardware-free build prints an explicit skip and --require-device fails closed.
add_executable(bloom_render_gpu_path_coverage_test
    gpu_path_coverage_tests.cpp
    gpu_path_coverage_parity_tests.cpp
    gpu_path_coverage_lifecycle_tests.cpp
    gpu_path_coverage_fault_seam.cpp
    gpu_path_coverage_test_support.hpp)
target_link_libraries(bloom_render_gpu_path_coverage_test PRIVATE bloom_render)
target_include_directories(bloom_render_gpu_path_coverage_test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan")
target_compile_definitions(bloom_render_gpu_path_coverage_test
    PRIVATE "BLOOM_PATH_COVERAGE_SPV_SHA256=\"f5aff832e47c64e42c95d0ae10429a5434e28566ccc1eb8d8c8d33e456bf2a4a\"")
bloom_enable_warnings(bloom_render_gpu_path_coverage_test)
bloom_enable_strict_floating_point(bloom_render_gpu_path_coverage_test)
bloom_add_test(
    NAME bloom.render.gpu_path_coverage
    COMMAND bloom_render_gpu_path_coverage_test
    LABELS unit gpu render
    TIMEOUT 300
)

# Host RGBA32F -> resident GpuImage upload test (staged host copy; no shader).
add_executable(bloom_render_gpu_image_upload_test gpu_image_upload_tests.cpp)
target_link_libraries(bloom_render_gpu_image_upload_test PRIVATE bloom_render)
bloom_enable_warnings(bloom_render_gpu_image_upload_test)
bloom_enable_strict_floating_point(bloom_render_gpu_image_upload_test)
bloom_add_test(
    NAME bloom.render.gpu_image_upload
    COMMAND bloom_render_gpu_image_upload_test
    LABELS unit gpu render
)

# Bounded-resource-retirement tests for GpuSolid/CoveredSolidV1 and GpuImageUpload. They include the
# Vulkan-free private fault seams, so the targets exist only where the Vulkan backend is built; a
# hardware-free build prints an explicit skip and --require-device fails closed. The vectors prove
# many pre-created instances beyond capacity, foreign-destruction boundedness, unproven-fence
# retention, exact owner-thread/generation isolation across two device threads, and owner-drain
# recovery.
if(BLOOM_RENDER_VULKAN_AVAILABLE)
    add_executable(bloom_render_gpu_solid_retirement_test gpu_solid_retirement_tests.cpp)
    target_link_libraries(bloom_render_gpu_solid_retirement_test PRIVATE bloom_render)
    target_include_directories(bloom_render_gpu_solid_retirement_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan")
    bloom_enable_warnings(bloom_render_gpu_solid_retirement_test)
    bloom_enable_strict_floating_point(bloom_render_gpu_solid_retirement_test)
    bloom_add_test(
        NAME bloom.render.gpu_solid_retirement
        COMMAND bloom_render_gpu_solid_retirement_test
        LABELS unit gpu render
        TIMEOUT 120
    )

    add_executable(bloom_render_gpu_image_upload_retirement_test
        gpu_image_upload_retirement_tests.cpp)
    target_link_libraries(bloom_render_gpu_image_upload_retirement_test PRIVATE bloom_render)
    target_include_directories(bloom_render_gpu_image_upload_retirement_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan")
    bloom_enable_warnings(bloom_render_gpu_image_upload_retirement_test)
    bloom_enable_strict_floating_point(bloom_render_gpu_image_upload_retirement_test)
    bloom_add_test(
        NAME bloom.render.gpu_image_upload_retirement
        COMMAND bloom_render_gpu_image_upload_retirement_test
        LABELS unit gpu render
        TIMEOUT 120
    )
endif()

# takeImage()-before-Ready guard regression for the three resident producers (upload, solid/covered,
# resident display), mirroring GpuComposite's readiness guard. It uses only the public render API, so
# it compiles and registers unconditionally against both the Vulkan bootstrap and the CPU stub; a
# hardware-free build prints an explicit device skip (and --require-device fails closed), while the
# invariant itself is exercised only on a real device.
add_executable(bloom_render_gpu_take_image_guard_test gpu_take_image_guard_tests.cpp)
target_link_libraries(bloom_render_gpu_take_image_guard_test PRIVATE bloom_render)
bloom_enable_warnings(bloom_render_gpu_take_image_guard_test)
bloom_enable_strict_floating_point(bloom_render_gpu_take_image_guard_test)
bloom_add_test(
    NAME bloom.render.gpu_take_image_guard
    COMMAND bloom_render_gpu_take_image_guard_test
    LABELS unit gpu render
)

# CPU-only kernel-prep test for TranslationOpacityBilinearV1 and SourceOverV1: hashes the embedded
# SPIR-V arrays against their pinned digests and derives every fixture expectation from the REAL CPU
# primitives. It never dispatches a GPU and registers unconditionally, so it also runs in a
# CPU-stub build.
add_executable(bloom_render_gpu_composite_kernel_test gpu_composite_kernel_tests.cpp)
target_link_libraries(bloom_render_gpu_composite_kernel_test PRIVATE bloom_render)
target_include_directories(bloom_render_gpu_composite_kernel_test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan")
target_compile_definitions(bloom_render_gpu_composite_kernel_test
    PRIVATE
        "BLOOM_TRANSLATION_OPACITY_SPV_SHA256=\"0049b132bf98214375023a82472f2839a0e8f30e1b1209a0f214ef283b97c3b8\""
        "BLOOM_SOURCE_OVER_SPV_SHA256=\"2aea19b4e3620e9e99f09f7f0e22f97822119ea798b44a1077179c4fc58194ee\"")
bloom_enable_warnings(bloom_render_gpu_composite_kernel_test)
bloom_enable_strict_floating_point(bloom_render_gpu_composite_kernel_test)
bloom_add_test(
    NAME bloom.render.gpu_composite_kernel
    COMMAND bloom_render_gpu_composite_kernel_test
    LABELS unit gpu render
)

# CPU-only kernel/formula test for BlendV1: hashes the embedded SPIR-V array against its pinned
# digest AND compares a Float32 host emulation of blend.comp to the real Float64 CPU primitive for
# all eight modes. It never dispatches a GPU and registers unconditionally, so it also runs in a
# CPU-stub build.
add_executable(bloom_render_gpu_blend_kernel_test gpu_blend_kernel_tests.cpp)
target_link_libraries(bloom_render_gpu_blend_kernel_test PRIVATE bloom_render)
target_include_directories(bloom_render_gpu_blend_kernel_test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan")
target_compile_definitions(bloom_render_gpu_blend_kernel_test
    PRIVATE
        "BLOOM_BLEND_SPV_SHA256=\"4b9cc2009f9a1fcbdc05558bbbbb9e9aeb7573a4fc88a8127e25e22bc6c208f8\""
        "BLOOM_BLEND_F64_SPV_SHA256=\"a910e190a05875c22f16d1571dea9e05496b32d5a588ede8cc8d95520ea7dbcf\""
        "BLOOM_BLEND_PORTABLE_SPV_SHA256=\"41c0272525b19c8dd43439244411b078e6c2936926fd8456c33fabf60cf68841\"")
bloom_enable_warnings(bloom_render_gpu_blend_kernel_test)
bloom_enable_strict_floating_point(bloom_render_gpu_blend_kernel_test)
bloom_add_test(
    NAME bloom.render.gpu_blend_kernel
    COMMAND bloom_render_gpu_blend_kernel_test
    LABELS unit gpu render
)

# Real-device native BlendV1 test for all eight core::BlendMode values with the CPU primitive as
# oracle, plus rejection, foreign-device, and cold/warm per-operation timing. It compiles the
# private Vulkan header via the composite support header, so the target exists only where the
# Vulkan backend is built; it skips cleanly without a device and --require-device fails closed.
if(BLOOM_RENDER_VULKAN_AVAILABLE)
    add_executable(bloom_render_gpu_blend_test
        gpu_blend_native_tests.cpp
        gpu_blend_portable_native_tests.cpp
        gpu_blend_benchmark_tests.cpp
        gpu_blend_native_support.hpp)
    target_link_libraries(bloom_render_gpu_blend_test PRIVATE bloom_render)
    target_include_directories(bloom_render_gpu_blend_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan" "${CMAKE_CURRENT_SOURCE_DIR}")
    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND IS_DIRECTORY "${BLOOM_DEPENDENCY_PREFIX}/include/vulkan")
        target_include_directories(bloom_render_gpu_blend_test
            SYSTEM PRIVATE "${BLOOM_DEPENDENCY_PREFIX}/include")
    endif()
    target_compile_definitions(bloom_render_gpu_blend_test
        PRIVATE
            "BLOOM_BLEND_SPV_SHA256=\"4b9cc2009f9a1fcbdc05558bbbbb9e9aeb7573a4fc88a8127e25e22bc6c208f8\""
            "BLOOM_BLEND_F64_SPV_SHA256=\"a910e190a05875c22f16d1571dea9e05496b32d5a588ede8cc8d95520ea7dbcf\""
            "BLOOM_BLEND_PORTABLE_SPV_SHA256=\"41c0272525b19c8dd43439244411b078e6c2936926fd8456c33fabf60cf68841\""
            BLOOM_RENDER_TEST_VULKAN_BACKEND=1)
    bloom_enable_warnings(bloom_render_gpu_blend_test)
    bloom_enable_strict_floating_point(bloom_render_gpu_blend_test)
    bloom_add_test(
        NAME bloom.render.gpu_blend
        COMMAND bloom_render_gpu_blend_test
        LABELS unit gpu render
    )
endif()

# Real-device native chain test (Solid -> TranslationOpacityBilinearV1 -> SourceOverV1) with the CPU
# primitives as oracle. It compiles the private Vulkan header via the composite support header, so
# the target exists only where the Vulkan backend is built; it skips cleanly without a device and
# --require-device fails closed. The same strict flags as the solid/resident tests apply.
if(BLOOM_RENDER_VULKAN_AVAILABLE)
    add_executable(bloom_render_gpu_composite_test gpu_composite_native_tests.cpp)
    target_link_libraries(bloom_render_gpu_composite_test PRIVATE bloom_render)
    target_include_directories(bloom_render_gpu_composite_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan" "${CMAKE_CURRENT_SOURCE_DIR}")
    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND IS_DIRECTORY "${BLOOM_DEPENDENCY_PREFIX}/include/vulkan")
        target_include_directories(bloom_render_gpu_composite_test
            SYSTEM PRIVATE "${BLOOM_DEPENDENCY_PREFIX}/include")
    endif()
    target_compile_definitions(bloom_render_gpu_composite_test
        PRIVATE
            "BLOOM_TRANSLATION_OPACITY_SPV_SHA256=\"0049b132bf98214375023a82472f2839a0e8f30e1b1209a0f214ef283b97c3b8\""
            "BLOOM_SOURCE_OVER_SPV_SHA256=\"2aea19b4e3620e9e99f09f7f0e22f97822119ea798b44a1077179c4fc58194ee\""
            BLOOM_RENDER_TEST_VULKAN_BACKEND=1)
    bloom_enable_warnings(bloom_render_gpu_composite_test)
    bloom_enable_strict_floating_point(bloom_render_gpu_composite_test)
    bloom_add_test(
        NAME bloom.render.gpu_composite
        COMMAND bloom_render_gpu_composite_test
        LABELS unit gpu render
    )

    # Production final-output readback primitive: real begin/poll/take, wrong-thread poll that
    # leaves the job Pending, and a foreign owner thread that cannot steal an active reservation.
    add_executable(bloom_render_gpu_process_readback_test gpu_process_readback_tests.cpp)
    target_link_libraries(bloom_render_gpu_process_readback_test PRIVATE bloom_render)
    target_include_directories(bloom_render_gpu_process_readback_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan" "${CMAKE_CURRENT_SOURCE_DIR}")
    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND IS_DIRECTORY "${BLOOM_DEPENDENCY_PREFIX}/include/vulkan")
        target_include_directories(bloom_render_gpu_process_readback_test
            SYSTEM PRIVATE "${BLOOM_DEPENDENCY_PREFIX}/include")
    endif()
    bloom_enable_warnings(bloom_render_gpu_process_readback_test)
    bloom_enable_strict_floating_point(bloom_render_gpu_process_readback_test)
    bloom_add_test(
        NAME bloom.render.gpu_process_readback
        COMMAND bloom_render_gpu_process_readback_test
        LABELS unit gpu render export
    )

    # Production combined output-colour readback: ONE submission carrying two distinct payloads
    # (process RGBA32F + encoded RGBA32F effect or RGBA8 display), process-only identity, pre-submit
    # budget refusal, and cancellation retirement.
    add_executable(bloom_render_gpu_output_color_readback_test
        gpu_output_color_readback_tests.cpp)
    target_link_libraries(bloom_render_gpu_output_color_readback_test PRIVATE bloom_render)
    target_include_directories(bloom_render_gpu_output_color_readback_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan" "${CMAKE_CURRENT_SOURCE_DIR}")
    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND IS_DIRECTORY "${BLOOM_DEPENDENCY_PREFIX}/include/vulkan")
        target_include_directories(bloom_render_gpu_output_color_readback_test
            SYSTEM PRIVATE "${BLOOM_DEPENDENCY_PREFIX}/include")
    endif()
    bloom_enable_warnings(bloom_render_gpu_output_color_readback_test)
    bloom_enable_strict_floating_point(bloom_render_gpu_output_color_readback_test)
    bloom_add_test(
        NAME bloom.render.gpu_output_color_readback
        COMMAND bloom_render_gpu_output_color_readback_test
        LABELS unit gpu render export color
    )

    # Deterministic fault injection for the combined readback reservation: forced allocation
    # failure, forced submit failure, forced fence timeout (bounded quarantine + refusal +
    # recovery), and foreign-thread destruction retention.
    add_executable(bloom_render_gpu_output_color_readback_fault_test
        gpu_output_color_readback_fault_tests.cpp)
    target_link_libraries(bloom_render_gpu_output_color_readback_fault_test
        PRIVATE bloom_render_output_color_readback_test_faults bloom_render)
    target_include_directories(bloom_render_gpu_output_color_readback_fault_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan" "${CMAKE_CURRENT_SOURCE_DIR}")
    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND IS_DIRECTORY "${BLOOM_DEPENDENCY_PREFIX}/include/vulkan")
        target_include_directories(bloom_render_gpu_output_color_readback_fault_test
            SYSTEM PRIVATE "${BLOOM_DEPENDENCY_PREFIX}/include")
    endif()
    bloom_enable_warnings(bloom_render_gpu_output_color_readback_fault_test)
    bloom_enable_strict_floating_point(bloom_render_gpu_output_color_readback_fault_test)
    bloom_add_test(
        NAME bloom.render.gpu_output_color_readback_fault
        COMMAND bloom_render_gpu_output_color_readback_fault_test
        LABELS unit gpu render export color
    )
endif()

# CPU-only kernel-prep and host-metadata test for AffineBilinearV1: hashes the embedded SPIR-V array
# against its pinned digest and derives every fixture expectation from the REAL CPU oracle. It never
# dispatches a GPU and registers unconditionally, so it also runs in a CPU-stub build.
add_executable(bloom_render_gpu_affine_kernel_test gpu_affine_kernel_tests.cpp)
target_link_libraries(bloom_render_gpu_affine_kernel_test PRIVATE bloom_render)
target_include_directories(bloom_render_gpu_affine_kernel_test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan")
target_compile_definitions(bloom_render_gpu_affine_kernel_test
    PRIVATE
        "BLOOM_AFFINE_BILINEAR_SPV_SHA256=\"3db9745132a3ee0721675dddf5f8d041f4e89eb4a2e5fed0410f80d6468facef\"")
bloom_enable_warnings(bloom_render_gpu_affine_kernel_test)
bloom_enable_strict_floating_point(bloom_render_gpu_affine_kernel_test)
bloom_add_test(
    NAME bloom.render.gpu_affine_kernel
    COMMAND bloom_render_gpu_affine_kernel_test
    LABELS unit gpu render
)

# Real-device native chain test (upload -> AffineBilinearV1) with the CPU primitives as oracle. It
# compiles the private Vulkan header via the affine private header, so the target exists only where
# the Vulkan backend is built; it skips cleanly without a device and --require-device fails closed.
if(BLOOM_RENDER_VULKAN_AVAILABLE)
    add_executable(bloom_render_gpu_affine_test gpu_affine_native_tests.cpp)
    target_link_libraries(bloom_render_gpu_affine_test PRIVATE bloom_render)
    target_include_directories(bloom_render_gpu_affine_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan" "${CMAKE_CURRENT_SOURCE_DIR}")
    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND IS_DIRECTORY "${BLOOM_DEPENDENCY_PREFIX}/include/vulkan")
        target_include_directories(bloom_render_gpu_affine_test
            SYSTEM PRIVATE "${BLOOM_DEPENDENCY_PREFIX}/include")
    endif()
    target_compile_definitions(bloom_render_gpu_affine_test
        PRIVATE
            "BLOOM_AFFINE_BILINEAR_SPV_SHA256=\"3db9745132a3ee0721675dddf5f8d041f4e89eb4a2e5fed0410f80d6468facef\""
            BLOOM_AFFINE_NATIVE_TEST=1
            BLOOM_RENDER_TEST_VULKAN_BACKEND=1)
    bloom_enable_warnings(bloom_render_gpu_affine_test)
    bloom_enable_strict_floating_point(bloom_render_gpu_affine_test)
    bloom_add_test(
        NAME bloom.render.gpu_affine
        COMMAND bloom_render_gpu_affine_test
        LABELS unit gpu render
    )

    # Honest cost measurement (host inverse-map metadata + dispatch + fence vs the CPU oracle at
    # 720p/1080p). Meaningful in a Release build; it prints the build mode it ran under.
    add_executable(bloom_render_gpu_affine_cost_test gpu_affine_cost_tests.cpp)
    target_link_libraries(bloom_render_gpu_affine_cost_test PRIVATE bloom_render)
    target_include_directories(bloom_render_gpu_affine_cost_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan" "${CMAKE_CURRENT_SOURCE_DIR}")
    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND IS_DIRECTORY "${BLOOM_DEPENDENCY_PREFIX}/include/vulkan")
        target_include_directories(bloom_render_gpu_affine_cost_test
            SYSTEM PRIVATE "${BLOOM_DEPENDENCY_PREFIX}/include")
    endif()
    bloom_enable_warnings(bloom_render_gpu_affine_cost_test)
    bloom_enable_strict_floating_point(bloom_render_gpu_affine_cost_test)
    bloom_add_test(
        NAME bloom.render.gpu_affine_cost
        COMMAND bloom_render_gpu_affine_cost_test
        LABELS benchmark gpu render
        TIMEOUT 300
    )
endif()

# CPU-only kernel-prep and host-metadata test for PointResampleV1: hashes the embedded SPIR-V array
# against its pinned digest and derives every axis-map expectation from an independent
# re-implementation of the media-image-proxy CPU oracle. It never dispatches a GPU and registers
# unconditionally, so it also runs in a CPU-stub build.
add_executable(bloom_render_gpu_point_resample_kernel_test gpu_point_resample_kernel_tests.cpp)
target_link_libraries(bloom_render_gpu_point_resample_kernel_test PRIVATE bloom_render)
target_include_directories(bloom_render_gpu_point_resample_kernel_test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan")
target_compile_definitions(bloom_render_gpu_point_resample_kernel_test
    PRIVATE
        "BLOOM_POINT_RESAMPLE_SPV_SHA256=\"4c053f341d4c23b84cf1717ef3d2a523347ae84f9e7638dd3c75410c9eeaa4a6\"")
bloom_enable_warnings(bloom_render_gpu_point_resample_kernel_test)
bloom_enable_strict_floating_point(bloom_render_gpu_point_resample_kernel_test)
bloom_add_test(
    NAME bloom.render.gpu_point_resample_kernel
    COMMAND bloom_render_gpu_point_resample_kernel_test
    LABELS unit gpu render
)

# Real-device native PointResampleV1 proof (upload -> point resample) with the media-image-proxy
# CPU oracle as reference. It compiles the private Vulkan header via the composite support header,
# so the target exists only where the Vulkan backend is built; it skips cleanly without a device and
# --require-device fails closed.
if(BLOOM_RENDER_VULKAN_AVAILABLE)
    add_executable(bloom_render_gpu_point_resample_test gpu_point_resample_native_tests.cpp)
    target_link_libraries(bloom_render_gpu_point_resample_test PRIVATE bloom_render)
    target_include_directories(bloom_render_gpu_point_resample_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan" "${CMAKE_CURRENT_SOURCE_DIR}")
    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND IS_DIRECTORY "${BLOOM_DEPENDENCY_PREFIX}/include/vulkan")
        target_include_directories(bloom_render_gpu_point_resample_test
            SYSTEM PRIVATE "${BLOOM_DEPENDENCY_PREFIX}/include")
    endif()
    target_compile_definitions(bloom_render_gpu_point_resample_test
        PRIVATE
            "BLOOM_POINT_RESAMPLE_SPV_SHA256=\"4c053f341d4c23b84cf1717ef3d2a523347ae84f9e7638dd3c75410c9eeaa4a6\""
            BLOOM_POINT_RESAMPLE_NATIVE_TEST=1
            BLOOM_RENDER_TEST_VULKAN_BACKEND=1)
    bloom_enable_warnings(bloom_render_gpu_point_resample_test)
    bloom_enable_strict_floating_point(bloom_render_gpu_point_resample_test)
    bloom_add_test(
        NAME bloom.render.gpu_point_resample
        COMMAND bloom_render_gpu_point_resample_test
        LABELS unit gpu render
    )

    # Deterministic lifecycle/boundedness fault tests for the PointResampleV1 reservation. Links the
    # test-only fault library; production targets never link it.
    add_executable(bloom_render_gpu_point_resample_fault_test gpu_point_resample_fault_tests.cpp)
    target_link_libraries(bloom_render_gpu_point_resample_fault_test
        PRIVATE bloom_render_point_resample_test_faults bloom_render)
    target_include_directories(bloom_render_gpu_point_resample_fault_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan" "${CMAKE_CURRENT_SOURCE_DIR}")
    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND IS_DIRECTORY "${BLOOM_DEPENDENCY_PREFIX}/include/vulkan")
        target_include_directories(bloom_render_gpu_point_resample_fault_test
            SYSTEM PRIVATE "${BLOOM_DEPENDENCY_PREFIX}/include")
    endif()
    bloom_enable_warnings(bloom_render_gpu_point_resample_fault_test)
    bloom_enable_strict_floating_point(bloom_render_gpu_point_resample_fault_test)
    bloom_add_test(
        NAME bloom.render.gpu_point_resample_fault
        COMMAND bloom_render_gpu_point_resample_fault_test
        LABELS unit gpu render
    )
endif()

# Pure CPU present-image test: the corrected fragment composition (display-space checkerboard,
# destination/source mapping, channel remap per texel BEFORE the filter, premultiplied bilinear
# filtering of the straight RGBA8 source, premultiplied overlay) reproduced on the CPU, plus the
# embedded viewer_present SPIR-V digest pins. No GPU, window, or Qt is needed, so it registers
# unconditionally and also runs in a CPU-stub build. The independent QPainter SmoothPixmapTransform
# oracle and the real-device offscreen proof live in the UI test directory (they need Qt).
add_executable(bloom_render_gpu_present_image_test gpu_present_image_tests.cpp)
target_link_libraries(bloom_render_gpu_present_image_test PRIVATE bloom_render)
target_include_directories(bloom_render_gpu_present_image_test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan")
bloom_enable_warnings(bloom_render_gpu_present_image_test)
bloom_enable_strict_floating_point(bloom_render_gpu_present_image_test)
bloom_add_test(
    NAME bloom.render.gpu_present_image
    COMMAND bloom_render_gpu_present_image_test
    LABELS unit gpu render
)

# Device-free presentation-target unit tests (format refusal table and wait-result
# classification). Only meaningful where the private Vulkan header is available.
if(BLOOM_RENDER_VULKAN_AVAILABLE)
    add_executable(bloom_render_gpu_presentation_target_test gpu_presentation_target_tests.cpp)
    target_link_libraries(bloom_render_gpu_presentation_target_test PRIVATE bloom_render)
    target_include_directories(bloom_render_gpu_presentation_target_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan")
    if(DEFINED BLOOM_DEPENDENCY_PREFIX AND IS_DIRECTORY "${BLOOM_DEPENDENCY_PREFIX}/include/vulkan")
        target_include_directories(bloom_render_gpu_presentation_target_test
            SYSTEM PRIVATE "${BLOOM_DEPENDENCY_PREFIX}/include")
    endif()
    bloom_enable_warnings(bloom_render_gpu_presentation_target_test)
    bloom_enable_strict_floating_point(bloom_render_gpu_presentation_target_test)
    bloom_add_test(
        NAME bloom.render.gpu_presentation_target
        COMMAND bloom_render_gpu_presentation_target_test
        LABELS unit gpu render
    )
endif()

# Test-only fault-instrumented copy of the executor's native GPU operation closure (VMA, device,
# image, solid + covered, composite, upload). It reuses the exact production private headers, sources,
# and strict compile profile, and is compiled only under the executor fault macro with the proof-only
# gpu_scene_executor_fault_injection.hpp on the include path. Because these objects define the same
# symbols first, the normal bloom_render native archive members are never pulled into the fault test,
# exactly as the external proof closure did; the installed production API and the default rebuild are
# unchanged. This target exists only where the Vulkan backend is built.
if(BLOOM_RENDER_VULKAN_AVAILABLE)
    add_library(bloom_render_gpu_scene_executor_fault OBJECT
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/vma.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/gpu_device.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/gpu_device_bootstrap.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/gpu_image.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/gpu_solid.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/gpu_solid_covered.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/gpu_solid_retirement.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/gpu_path_coverage.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/gpu_path_coverage_lifecycle.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/gpu_path_coverage_resources.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/gpu_composite.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/gpu_composite_resources.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/gpu_image_upload.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/gpu_image_upload_resources.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/gpu_image_upload_retirement.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/gpu_affine.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/gpu_affine_resources.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/gpu_blend.cpp"
    )
    target_include_directories(bloom_render_gpu_scene_executor_fault PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../core/include"
    )
    target_compile_features(bloom_render_gpu_scene_executor_fault PRIVATE cxx_std_20)
    target_compile_definitions(bloom_render_gpu_scene_executor_fault
        PRIVATE BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION)
    target_link_libraries(bloom_render_gpu_scene_executor_fault
        PRIVATE Vulkan::Headers GPUOpen::VulkanMemoryAllocator)
    bloom_enable_warnings(bloom_render_gpu_scene_executor_fault)
    bloom_enable_strict_floating_point(bloom_render_gpu_scene_executor_fault)
endif()
