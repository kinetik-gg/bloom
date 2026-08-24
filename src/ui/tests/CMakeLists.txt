add_executable(bloom_composition_projection_test
    composition_projection_test.cpp
)

target_link_libraries(bloom_composition_projection_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_composition_projection_test)

include(BloomTesting)

bloom_add_test(
    NAME bloom.ui.composition_projection
    COMMAND bloom_composition_projection_test
    LABELS ui integration
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
