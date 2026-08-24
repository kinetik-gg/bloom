add_executable(bloom_commands_test
    command_stack_tests.cpp
)

add_executable(bloom_commands_structured_layer_test
    structured_layer_tests.cpp
)

add_executable(bloom_commands_animation_test
    animation_operations_tests.cpp
)

target_link_libraries(bloom_commands_test PRIVATE bloom_commands)
bloom_enable_warnings(bloom_commands_test)

target_link_libraries(bloom_commands_structured_layer_test PRIVATE bloom_commands)
bloom_enable_warnings(bloom_commands_structured_layer_test)

target_link_libraries(bloom_commands_animation_test PRIVATE bloom_commands)
bloom_enable_warnings(bloom_commands_animation_test)

include(BloomTesting)

bloom_add_test(
    NAME bloom.commands
    COMMAND bloom_commands_test
    LABELS commands unit
    TIMEOUT 30
)

bloom_add_test(
    NAME bloom.commands.structured-layers
    COMMAND bloom_commands_structured_layer_test
    LABELS commands unit
    TIMEOUT 30
)

bloom_add_test(
    NAME bloom.commands.animation
    COMMAND bloom_commands_animation_test
    LABELS animation commands unit
    TIMEOUT 30
)
