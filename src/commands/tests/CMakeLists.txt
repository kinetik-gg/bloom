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

add_executable(bloom_commands_node_test node_operations_tests.cpp)
target_link_libraries(bloom_commands_node_test PRIVATE bloom_commands)
bloom_enable_warnings(bloom_commands_node_test)
bloom_add_test(NAME bloom.commands.nodes COMMAND bloom_commands_node_test LABELS commands unit)

add_executable(bloom_commands_composition_test composition_operations_tests.cpp)
target_link_libraries(bloom_commands_composition_test PRIVATE bloom_commands)
bloom_enable_warnings(bloom_commands_composition_test)
bloom_add_test(NAME bloom.commands.compositions COMMAND bloom_commands_composition_test LABELS commands unit)

add_executable(bloom_asset_operations_test ${PROJECT_SOURCE_DIR}/tests/integration/media_asset_operations_tests.cpp)
target_include_directories(bloom_asset_operations_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bloom_asset_operations_test PRIVATE bloom_commands bloom_project)
bloom_enable_warnings(bloom_asset_operations_test)
bloom_add_test(NAME bloom.commands.assets COMMAND bloom_asset_operations_test LABELS unit commands media)

add_executable(bloom_commands_asset_organization_test asset_organization_tests.cpp)
target_link_libraries(bloom_commands_asset_organization_test PRIVATE bloom_commands)
bloom_enable_warnings(bloom_commands_asset_organization_test)
bloom_add_test(NAME bloom.commands.asset-organization COMMAND bloom_commands_asset_organization_test LABELS unit commands)

# NETSHARE-1 (locator robustness): imports from, relinks within, and save/open round-trips a
# simulated network-share mount (a temporary directory outside the project directory). Lives
# under tests/integration/ rather than here (mirroring bloom_asset_operations_test just below)
# because it deliberately crosses into bloom/project for the encode/decode/reconstruct round
# trip, a dependency src/commands itself may not declare. Its own self-contained one-pixel PNG
# writer links ZLIB::ZLIB directly for deflate/CRC32, mirroring src/project/CMakeLists.txt's and
# src/host/CMakeLists.txt's existing from-scratch PNG/ZIP test fixtures rather than depending on
# another module's test-only helpers.
if(NOT TARGET ZLIB::ZLIB)
    find_package(ZLIB REQUIRED)
endif()
add_executable(bloom_commands_network_share_locator_test
    ${PROJECT_SOURCE_DIR}/tests/integration/network_share_locator_tests.cpp)
target_include_directories(bloom_commands_network_share_locator_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bloom_commands_network_share_locator_test
    PRIVATE bloom_commands bloom_project bloom_media_image ZLIB::ZLIB
)
bloom_enable_warnings(bloom_commands_network_share_locator_test)
bloom_add_test(
    NAME bloom.commands.network-share-locator
    COMMAND bloom_commands_network_share_locator_test
    LABELS unit commands media
)
