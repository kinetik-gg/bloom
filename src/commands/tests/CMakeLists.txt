add_executable(bloom_commands_test
    commands_test.cpp
)

target_link_libraries(bloom_commands_test PRIVATE bloom_commands)
bloom_enable_warnings(bloom_commands_test)

include(BloomTesting)

bloom_add_test(
    NAME bloom.commands
    COMMAND bloom_commands_test
    LABELS commands unit
    TIMEOUT 30
)
