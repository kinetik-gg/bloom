add_library(bloom_ui STATIC)

target_sources(
    bloom_ui
    PRIVATE
        composition_editors.cpp
        composition_session.cpp
        editor_area.cpp
        editor_registry.cpp
        main_window.cpp
        node_editor.cpp
        workspace_host.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/ui/composition_editors.hpp
            include/bloom/ui/composition_session.hpp
            include/bloom/ui/editor_area.hpp
            include/bloom/ui/editor_registry.hpp
            include/bloom/ui/main_window.hpp
            include/bloom/ui/node_editor.hpp
            include/bloom/ui/workspace_host.hpp
)

target_compile_features(bloom_ui PUBLIC cxx_std_20)
target_link_libraries(bloom_ui PUBLIC bloom_commands Qt6::Widgets)
bloom_enable_warnings(bloom_ui)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
