add_library(
    bloom_quality_json
    STATIC
    strict_json.cpp
    strict_json.hpp
)
target_include_directories(bloom_quality_json PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(bloom_quality_json PUBLIC cxx_std_20)
bloom_enable_warnings(bloom_quality_json)
