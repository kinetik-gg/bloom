# Vulkan-Loader vulkan-sdk-1.4.357.0 - the runtime Vulkan loader (Apache-2.0-led; see
# dependencies/licenses/vulkan-loader/). It builds the shared, end-user runtime library
# libvulkan.so.1 and depends on the accepted locked Vulkan-Headers component for its build.
#
# Wayland presentation loader for the GPU-resident viewer slice. The running desktop is Hyprland
# (WAYLAND_DISPLAY=wayland-1, QT_QPA_PLATFORM=wayland; the live Bloom process loads
# Qt6WaylandClient 6.11.2), so the smallest WSI configuration consistent with the actual platform is
# VK_KHR_wayland_surface alone: on this loader version the Linux/BSD Wayland branch only defines
# VK_USE_PLATFORM_WAYLAND_KHR and calls no pkg-config, so the build gains no xcb/x11/xrandr or
# wayland-client dependency and the installed libvulkan.so.1 gains no window-system DT_NEEDED
# (verified: the current compute-only loader's only NEEDED entries are libm/libc).
#
# XCB/Xlib/Xrandr WSI stay explicitly OFF. They are functional on this host (pkg-config xcb 1.17.0,
# x11 1.8.13, xrandr 1.5.5) but those host dev packages are not pinned in the qualified profile, so
# an XCB QPA (XWayland fallback) presents no GPU surface and uses the CPU QImage path until a
# separate xcb/x11/xrandr dependency intake lands. On Windows the loader defines
# VK_USE_PLATFORM_WIN32_KHR unconditionally and on Apple it defines the Metal/MacOS portability
# surfaces; this recipe deliberately does not override or mask those platform branches.
#
# BUILD_TESTS=OFF keeps the test tree and its MIT-Khronos-old sources unreached; LOADER_CODEGEN=OFF
# uses the checked-in generated sources and needs no Python code generation; UPDATE_DEPS=OFF
# disables the loader's own dependency fetcher, so the build performs no implicit fetching. The
# recipe depends on bloom_dependency_vulkan-headers and resolves Vulkan::Headers from the shared
# prefix through CMAKE_PREFIX_PATH.
set(BLOOM_VULKAN_LOADER_VERSION vulkan-sdk-1.4.357.0)
set(BLOOM_VULKAN_LOADER_URL
    "https://codeload.github.com/KhronosGroup/Vulkan-Loader/tar.gz/refs/tags/vulkan-sdk-1.4.357.0")
set(BLOOM_VULKAN_LOADER_SHA256
    54f2537df22313768da0317dda2abdaaab7711b4081c48c869a79db343d0ae70)

set(BLOOM_VULKAN_LOADER_WSI_ARGS)
if(CMAKE_SYSTEM_NAME MATCHES "Linux|BSD|DragonFly|GNU|CYGWIN")
    set(BLOOM_VULKAN_LOADER_WSI_ARGS
        -DBUILD_WSI_XCB_SUPPORT:BOOL=OFF
        -DBUILD_WSI_XLIB_SUPPORT:BOOL=OFF
        -DBUILD_WSI_XLIB_XRANDR_SUPPORT:BOOL=OFF
        -DBUILD_WSI_WAYLAND_SUPPORT:BOOL=ON
        -DBUILD_WSI_DIRECTFB_SUPPORT:BOOL=OFF)
endif()

ExternalProject_Add(bloom_dependency_vulkan-loader
    URL "${BLOOM_VULKAN_LOADER_URL}"
    URL_HASH SHA256=${BLOOM_VULKAN_LOADER_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME vulkan-loader-vulkan-sdk-1.4.357.0.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    DEPENDS
        bloom_dependency_vulkan-headers
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DCMAKE_PREFIX_PATH:PATH=${BLOOM_DEPENDENCY_PREFIX}
        -DBUILD_TESTS:BOOL=OFF
        -DLOADER_CODEGEN:BOOL=OFF
        -DUPDATE_DEPS:BOOL=OFF
        ${BLOOM_VULKAN_LOADER_WSI_ARGS}
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
