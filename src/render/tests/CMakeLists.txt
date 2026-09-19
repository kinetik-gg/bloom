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
