add_library(bloom_render STATIC)

target_sources(
    bloom_render
    PRIVATE
        display_buffer.cpp
        image.cpp
        image_types.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/render/display_buffer.hpp
            include/bloom/render/image.hpp
            include/bloom/render/image_types.hpp
)

target_compile_features(bloom_render PUBLIC cxx_std_20)
target_link_libraries(bloom_render PUBLIC bloom_core)
bloom_enable_warnings(bloom_render)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
