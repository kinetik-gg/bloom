# Vulkan-Headers vulkan-sdk-1.4.357.0 - the canonical Khronos Vulkan C headers plus the generated
# Vulkan-Hpp C++ headers. docs/architecture/gpu-backend.md names Vulkan-Hpp as a tracked dependency;
# the supervisor's primary-source check (registry.khronos.org/vulkan/) confirms the generated
# vulkan.hpp is distributed inside the Vulkan-Headers repository, so there is deliberately no
# separate Vulkan-Hpp component in this intake.
#
# Header/data-only install: VULKAN_HEADERS_ENABLE_TESTS is forced OFF because the ExternalProject
# configures this tree as top level, where the upstream default would enable the test suite.
# VULKAN_HEADERS_ENABLE_MODULE is forced OFF so the build-toolchain-specific Vulkan-Hpp C++ named
# module is not built or installed; Bloom consumes the classic headers. VULKAN_HEADERS_ENABLE_INSTALL
# installs include/vulkan, include/vk_video, the XML registry, and VulkanHeadersConfig.cmake (target
# Vulkan::Headers) into the shared prefix for a later restricted find_package.
#
# No in-recipe fetch: the tag archive carries no submodule content and the build phase runs offline.
set(BLOOM_VULKAN_HEADERS_VERSION vulkan-sdk-1.4.357.0)
set(BLOOM_VULKAN_HEADERS_URL
    "https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/refs/tags/vulkan-sdk-1.4.357.0")
set(BLOOM_VULKAN_HEADERS_SHA256
    e87dce08116151f6b6d7de6b6faf41498e87e6cf848ff16fa3bd5402190ad4a3)

ExternalProject_Add(bloom_dependency_vulkan-headers
    URL "${BLOOM_VULKAN_HEADERS_URL}"
    URL_HASH SHA256=${BLOOM_VULKAN_HEADERS_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME vulkan-headers-vulkan-sdk-1.4.357.0.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DVULKAN_HEADERS_ENABLE_TESTS:BOOL=OFF
        -DVULKAN_HEADERS_ENABLE_INSTALL:BOOL=ON
        -DVULKAN_HEADERS_ENABLE_MODULE:BOOL=OFF
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
