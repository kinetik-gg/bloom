add_library(bloom_core STATIC)

target_sources(
    bloom_core
    PRIVATE
        rational_time.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/core/color.hpp
            include/bloom/core/id.hpp
            include/bloom/core/pixel_aspect_ratio.hpp
            include/bloom/core/rational_time.hpp
)

target_compile_features(bloom_core PUBLIC cxx_std_20)
bloom_enable_warnings(bloom_core)

if(BUILD_TESTING)
    include(BloomTesting)
    add_executable(bloom_core_pixel_aspect_ratio_test tests/pixel_aspect_ratio_tests.cpp)
    target_link_libraries(bloom_core_pixel_aspect_ratio_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_pixel_aspect_ratio_test)
    bloom_add_test(
        NAME bloom.core.pixel_aspect_ratio
        COMMAND bloom_core_pixel_aspect_ratio_test
        LABELS unit
    )
endif()
