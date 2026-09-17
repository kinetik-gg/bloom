add_library(bloom_media_cache STATIC)

target_sources(
    bloom_media_cache
    PRIVATE
        media_disk_cache.cpp
        media_disk_cache_decode.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/media/cache/media_disk_cache.hpp
            include/bloom/media/cache/media_disk_cache_decode.hpp
)

target_compile_features(bloom_media_cache PUBLIC cxx_std_20)
target_link_libraries(bloom_media_cache PUBLIC bloom_core bloom_render bloom_media_image)
bloom_enable_warnings(bloom_media_cache)
bloom_enable_strict_floating_point(bloom_media_cache)

if(BUILD_TESTING)
    include(BloomTesting)
    bloom_find_dependency(ZLIB)
    add_executable(bloom_media_cache_test tests/media_disk_cache_tests.cpp)
    # ZLIB/bloom_output/the src/output/tests include path are needed only to reuse the existing
    # independent PngChunkBytes fixture writer (png_test_support.hpp, the same helper
    # src/media/image/tests/image_tests.cpp already uses) for the two tests that exercise the real
    # decodeThroughDiskCache() entry point end to end; every other test below builds its fixture
    # images directly through Rgba32fImageBuilder.
    target_link_libraries(bloom_media_cache_test PRIVATE bloom_media_cache bloom_output
                                                          ZLIB::ZLIBSTATIC)
    target_include_directories(bloom_media_cache_test PRIVATE ${PROJECT_SOURCE_DIR}/src/output/tests)
    bloom_enable_warnings(bloom_media_cache_test)
    bloom_add_test(NAME bloom.media.cache COMMAND bloom_media_cache_test LABELS unit media cache
                  TIMEOUT 60)
endif()
