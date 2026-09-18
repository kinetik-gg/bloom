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

if(APPLE)
    # Self-contained .app: copy the Qt frameworks (and plugins) the desktop needs beside the
    # executable. Optional -- it requires Qt's macdeployqt, which is present in a normal Qt install
    # but not in a headless CI checkout that only links Qt -- so the target appears only when found.
    get_target_property(bloom_qmake Qt6::qmake IMPORTED_LOCATION)
    if(bloom_qmake)
        get_filename_component(bloom_qt_bin "${bloom_qmake}" DIRECTORY)
        find_program(BLOOM_MACDEPLOYQT NAMES macdeployqt HINTS "${bloom_qt_bin}")
        if(BLOOM_MACDEPLOYQT)
            add_custom_target(
                bloom-deploy-macos
                COMMAND "${BLOOM_MACDEPLOYQT}" "$<TARGET_BUNDLE_DIR:bloom>" -always-overwrite
                DEPENDS bloom
                COMMENT "Bundling Qt frameworks into Bloom.app with macdeployqt"
                VERBATIM
            )
        endif()
    endif()
endif()
