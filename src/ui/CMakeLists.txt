add_library(bloom_ui STATIC)

target_sources(
    bloom_ui
    PRIVATE
        main_window.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES include/bloom/ui/main_window.hpp
)

target_compile_features(bloom_ui PUBLIC cxx_std_20)
target_link_libraries(bloom_ui PUBLIC Qt6::Widgets)
bloom_enable_warnings(bloom_ui)
