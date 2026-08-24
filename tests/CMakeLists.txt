add_executable(bloom_ui_smoke_test
    ui_smoke.cpp
)

target_link_libraries(bloom_ui_smoke_test PRIVATE bloom_ui)
bloom_enable_warnings(bloom_ui_smoke_test)

add_test(NAME bloom.ui.smoke COMMAND bloom_ui_smoke_test)
set_tests_properties(
    bloom.ui.smoke
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
