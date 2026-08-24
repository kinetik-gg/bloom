add_library(bloom_core STATIC)

target_sources(
    bloom_core
    PRIVATE
        rational_time.cpp
        rational_interval.cpp
        frame_time_mapping.cpp
        scalar_primitives.cpp
        sha256.cpp
        utf8.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/core/artifact_target_key.hpp
            include/bloom/core/color.hpp
            include/bloom/core/floating_point.hpp
            include/bloom/core/frame_time_mapping.hpp
            include/bloom/core/id.hpp
            include/bloom/core/pixel_aspect_ratio.hpp
            include/bloom/core/rational_interval.hpp
            include/bloom/core/rational_time.hpp
            include/bloom/core/scalar_primitives.hpp
            include/bloom/core/sha256.hpp
            include/bloom/core/utf8.hpp
)

target_compile_features(bloom_core PUBLIC cxx_std_20)
bloom_enable_warnings(bloom_core)

# Scalar primitives are the backend-neutral CPU reference. Keep ordinary expressions unfused,
# honor the runtime rounding mode check, and reserve fused semantics for explicit std::fma calls.
bloom_enable_strict_floating_point(bloom_core)

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

    add_executable(bloom_core_rational_interval_test tests/rational_interval_tests.cpp)
    target_link_libraries(bloom_core_rational_interval_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_rational_interval_test)
    bloom_enable_strict_floating_point(bloom_core_rational_interval_test)
    bloom_add_test(
        NAME bloom.core.rational_interval
        COMMAND bloom_core_rational_interval_test
        LABELS unit primitives
    )

    add_executable(bloom_core_frame_time_mapping_test tests/frame_time_mapping_tests.cpp)
    target_link_libraries(bloom_core_frame_time_mapping_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_frame_time_mapping_test)
    bloom_add_test(
        NAME bloom.core.frame_time_mapping
        COMMAND bloom_core_frame_time_mapping_test
        LABELS unit time
    )

    add_executable(bloom_core_scalar_primitives_test tests/scalar_primitives_tests.cpp)
    target_link_libraries(bloom_core_scalar_primitives_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_scalar_primitives_test)
    bloom_enable_strict_floating_point(bloom_core_scalar_primitives_test)
    bloom_add_test(
        NAME bloom.core.scalar_primitives
        COMMAND bloom_core_scalar_primitives_test
        LABELS unit primitives
    )

    add_executable(bloom_core_sha256_test tests/sha256_tests.cpp)
    target_link_libraries(bloom_core_sha256_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_sha256_test)
    bloom_add_test(
        NAME bloom.core.sha256
        COMMAND bloom_core_sha256_test
        LABELS unit identity
    )

    add_executable(bloom_core_utf8_test tests/utf8_tests.cpp)
    target_link_libraries(bloom_core_utf8_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_utf8_test)
    bloom_add_test(NAME bloom.core.utf8 COMMAND bloom_core_utf8_test LABELS unit text)
endif()
