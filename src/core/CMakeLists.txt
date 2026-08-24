add_library(bloom_core STATIC)

target_sources(
    bloom_core
    PRIVATE
        rational_time.cpp
        scalar_primitives.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/core/color.hpp
            include/bloom/core/id.hpp
            include/bloom/core/pixel_aspect_ratio.hpp
            include/bloom/core/rational_time.hpp
            include/bloom/core/scalar_primitives.hpp
)

target_compile_features(bloom_core PUBLIC cxx_std_20)
bloom_enable_warnings(bloom_core)

# Scalar primitives are the backend-neutral CPU reference. Keep ordinary expressions unfused,
# honor the runtime rounding mode check, and reserve fused semantics for explicit std::fma calls.
if(MSVC)
    target_compile_options(bloom_core PRIVATE /fp:strict)
else()
    target_compile_options(bloom_core PRIVATE -fno-fast-math -ffp-contract=off -frounding-math)
endif()

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

    add_executable(bloom_core_scalar_primitives_test tests/scalar_primitives_tests.cpp)
    target_link_libraries(bloom_core_scalar_primitives_test PRIVATE bloom_core)
    bloom_enable_warnings(bloom_core_scalar_primitives_test)
    if(MSVC)
        target_compile_options(bloom_core_scalar_primitives_test PRIVATE /fp:strict)
    else()
        target_compile_options(
            bloom_core_scalar_primitives_test
            PRIVATE -fno-fast-math -ffp-contract=off -frounding-math
        )
    endif()
    bloom_add_test(
        NAME bloom.core.scalar_primitives
        COMMAND bloom_core_scalar_primitives_test
        LABELS unit primitives
    )
endif()
