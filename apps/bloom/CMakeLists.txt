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

# --- Bundled compute-only Vulkan loader (Linux native-backend builds only) -----------------------
#
# The device path loads the loader dynamically instead of linking it, so no DT_NEEDED entry makes
# it a hard runtime requirement. Only the validated qualified prefix may supply the bundled loader:
# this imports VulkanLoader from BLOOM_DEPENDENCY_PREFIX with NO_DEFAULT_PATH and never resolves a
# host or developer-workspace loader. When the native backend is built and that exact prefix
# provides one, the loader is copied into the app-relative "bloom-vulkan" directory and installed
# to the same relative place. main.cpp resolves it from the executable's own directory and nothing
# else. macOS, Windows, stub, developer-system, and missing-loader builds get no definition, so
# main.cpp takes the explicit CPU fallback and never sets up a GPU preference.
set(BLOOM_APP_BUNDLED_VULKAN_LOADER OFF)
if(UNIX
   AND NOT APPLE
   AND BLOOM_RENDER_ENABLE_VULKAN
   AND DEFINED BLOOM_DEPENDENCY_MODE
   AND BLOOM_DEPENDENCY_MODE STREQUAL "qualified")
    find_package(VulkanHeaders QUIET CONFIG PATHS "${BLOOM_DEPENDENCY_PREFIX}" NO_DEFAULT_PATH)
    find_package(VulkanMemoryAllocator QUIET CONFIG PATHS "${BLOOM_DEPENDENCY_PREFIX}"
        NO_DEFAULT_PATH)
    find_package(VulkanLoader QUIET CONFIG PATHS "${BLOOM_DEPENDENCY_PREFIX}" NO_DEFAULT_PATH)
    if(TARGET Vulkan::Headers AND TARGET GPUOpen::VulkanMemoryAllocator AND TARGET Vulkan::Loader)
        set(BLOOM_APP_BUNDLED_VULKAN_LOADER ON)
    endif()
endif()

if(BLOOM_APP_BUNDLED_VULKAN_LOADER)
    set(BLOOM_BUNDLED_VULKAN_LOADER_SUBDIR "bloom-vulkan")
    set(BLOOM_BUNDLED_VULKAN_LOADER_NAME "libvulkan.so.1")
    # Always-run refresh (not only POST_BUILD): the bundled loader is re-copied whenever its
    # content changes, even when the application itself does not relink. copy_if_different keeps
    # an unchanged loader from touching the app directory.
    add_custom_target(
        bloom-bundled-vulkan-loader ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:bloom>/${BLOOM_BUNDLED_VULKAN_LOADER_SUBDIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Vulkan::Loader>"
            "$<TARGET_FILE_DIR:bloom>/${BLOOM_BUNDLED_VULKAN_LOADER_SUBDIR}/${BLOOM_BUNDLED_VULKAN_LOADER_NAME}"
        DEPENDS Vulkan::Loader
        COMMENT "Refreshing the validated Vulkan loader beside the Bloom executable"
        VERBATIM
    )
    add_dependencies(bloom bloom-bundled-vulkan-loader)
    install(
        FILES "$<TARGET_FILE:Vulkan::Loader>"
        DESTINATION "bin/${BLOOM_BUNDLED_VULKAN_LOADER_SUBDIR}"
        RENAME "${BLOOM_BUNDLED_VULKAN_LOADER_NAME}"
    )
    target_compile_definitions(
        bloom
        PRIVATE
            BLOOM_BUNDLED_VULKAN_LOADER=1
            BLOOM_BUNDLED_VULKAN_LOADER_SUBDIR="${BLOOM_BUNDLED_VULKAN_LOADER_SUBDIR}"
            BLOOM_BUNDLED_VULKAN_LOADER_NAME="${BLOOM_BUNDLED_VULKAN_LOADER_NAME}"
    )
endif()

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
