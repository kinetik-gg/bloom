qt_add_executable(bloom
    main.cpp
)

target_link_libraries(bloom PRIVATE bloom_runtime bloom_runtime_compiler bloom_ui)
bloom_enable_warnings(bloom)

set_target_properties(
    bloom
    PROPERTIES
        OUTPUT_NAME Bloom
        WIN32_EXECUTABLE TRUE
        MACOSX_BUNDLE TRUE
)

install(
    TARGETS bloom
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
