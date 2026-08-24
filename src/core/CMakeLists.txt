add_library(bloom_core STATIC)

target_sources(
    bloom_core
    PRIVATE
        rational_time.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/core/id.hpp
            include/bloom/core/rational_time.hpp
)

target_compile_features(bloom_core PUBLIC cxx_std_20)
bloom_enable_warnings(bloom_core)
