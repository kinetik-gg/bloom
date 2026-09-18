add_library(bloom_commands STATIC)

target_sources(
    bloom_commands
    PRIVATE
        animation_operations.cpp
        command_stack.cpp
        operations.cpp
        asset_operations.cpp
        asset_organization_operations.cpp
        node_operations.cpp
        layer_operations.cpp
        node_duplication.cpp
        node_group_operations.cpp
        node_layout_operations.cpp
        result.cpp
        transaction.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bloom/commands/animation_operations.hpp
            include/bloom/commands/command_stack.hpp
            include/bloom/commands/operation.hpp
            include/bloom/commands/operations.hpp
            include/bloom/commands/asset_operations.hpp
            include/bloom/commands/node_operations.hpp
            include/bloom/commands/layer_operations.hpp
            include/bloom/commands/result.hpp
            include/bloom/commands/transaction.hpp
)

target_compile_features(bloom_commands PUBLIC cxx_std_20)
target_link_libraries(bloom_commands PUBLIC bloom_document bloom_media_provider
                      PRIVATE bloom_media_image bloom_media_audio bloom_media_video)
bloom_enable_warnings(bloom_commands)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
