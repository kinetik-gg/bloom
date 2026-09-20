add_library(bloom_media_stb_image OBJECT stb_image_adapter.cpp stb_image_adapter.hpp)
target_include_directories(bloom_media_stb_image PRIVATE ../third_party/stb_image)
target_compile_features(bloom_media_stb_image PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(bloom_media_stb_image PRIVATE /w)
else()
    target_compile_options(bloom_media_stb_image PRIVATE -w)
endif()
bloom_enable_strict_floating_point(bloom_media_stb_image)

add_library(bloom_media_image STATIC exr_backend.cpp exr_backend.hpp image.cpp image_budget.hpp
    sequence.cpp)
target_sources(bloom_media_image PUBLIC FILE_SET HEADERS BASE_DIRS include
    FILES include/bloom/media/image.hpp)
target_compile_features(bloom_media_image PUBLIC cxx_std_20)
target_link_libraries(bloom_media_image PUBLIC bloom_core bloom_render bloom_color_ocio
    PRIVATE bloom_media_stb_image)
bloom_find_dependency(OpenEXR)
target_link_libraries(bloom_media_image PRIVATE OpenEXR::OpenEXR)
bloom_enable_warnings(bloom_media_image)
bloom_enable_strict_floating_point(bloom_media_image)

if(BUILD_TESTING)
    include(BloomTesting)
    bloom_find_dependency(ZLIB)
    add_executable(bloom_media_image_test tests/image_tests.cpp)
    target_link_libraries(bloom_media_image_test PRIVATE bloom_media_image bloom_output ZLIB::ZLIBSTATIC
        OpenEXR::OpenEXR)
    target_include_directories(bloom_media_image_test PRIVATE ${PROJECT_SOURCE_DIR}/src/output/tests)
    bloom_enable_warnings(bloom_media_image_test)
    bloom_add_test(NAME bloom.media.image COMMAND bloom_media_image_test LABELS unit media)

    add_executable(bloom_media_image_capacity_test tests/image_capacity_tests.cpp)
    target_link_libraries(bloom_media_image_capacity_test PRIVATE bloom_media_image bloom_output
        ZLIB::ZLIBSTATIC OpenEXR::OpenEXR)
    target_include_directories(bloom_media_image_capacity_test PRIVATE ${PROJECT_SOURCE_DIR}/src/output/tests
        ${CMAKE_CURRENT_SOURCE_DIR})
    bloom_enable_warnings(bloom_media_image_capacity_test)
    bloom_add_test(NAME bloom.media.image_capacity COMMAND bloom_media_image_capacity_test LABELS unit media)
endif()
