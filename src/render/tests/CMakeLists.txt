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

# The neutral display test links bloom_color_ocio only for the independent CPU oracle. The render
# module itself knows nothing about OCIO; the GPU operation and the oracle are compared through the
# shared Bloom image types. It also includes the private SPIR-V array directly to prove the checked-in
# words hash to the pinned SPIR-V digest, so a changed array word fails even if comments are intact.
add_executable(bloom_render_gpu_neutral_display_test gpu_neutral_display_tests.cpp)
target_link_libraries(bloom_render_gpu_neutral_display_test PRIVATE bloom_render bloom_color_ocio)
target_include_directories(bloom_render_gpu_neutral_display_test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan")
target_compile_definitions(bloom_render_gpu_neutral_display_test
    PRIVATE "BLOOM_NEUTRAL_DISPLAY_SPV_SHA256=\"fc0a97d36c42aafd3cfce56dcfdd7ff3c75d3ec7fd0e52a83c7f272c54feb3f9\"")
if(BLOOM_RENDER_VULKAN_AVAILABLE)
    target_compile_definitions(bloom_render_gpu_neutral_display_test
        PRIVATE BLOOM_RENDER_TEST_VULKAN_BACKEND=1)
endif()
bloom_enable_warnings(bloom_render_gpu_neutral_display_test)
bloom_enable_strict_floating_point(bloom_render_gpu_neutral_display_test)
bloom_add_test(
    NAME bloom.render.gpu_neutral_display
    COMMAND bloom_render_gpu_neutral_display_test
    LABELS unit gpu color
)

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

# GPU-resident Neutral V1 display test. It links bloom_color_ocio only for the independent CPU
# oracle; the render module itself stays OCIO-free.
add_executable(bloom_render_gpu_resident_display_test gpu_resident_display_tests.cpp)
target_link_libraries(bloom_render_gpu_resident_display_test PRIVATE bloom_render bloom_color_ocio)
bloom_enable_warnings(bloom_render_gpu_resident_display_test)
bloom_enable_strict_floating_point(bloom_render_gpu_resident_display_test)
bloom_add_test(
    NAME bloom.render.gpu_resident_display
    COMMAND bloom_render_gpu_resident_display_test
    LABELS unit gpu color
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
endif()

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
