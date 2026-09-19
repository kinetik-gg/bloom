# VulkanMemoryAllocator v3.4.0 - the single-header Vulkan memory allocator (MIT, AMD).
# docs/architecture/gpu-backend.md names VMA as a tracked dependency, wrapped behind Bloom-owned
# resource types rather than exposed publicly.
#
# Header-only install: VMA builds an INTERFACE target (include/vk_mem_alloc.h); VMA_ENABLE_INSTALL
# installs the header, the exported target GPUOpen::VulkanMemoryAllocator, and
# VulkanMemoryAllocatorConfig.cmake into the shared prefix. VMA_BUILD_DOCUMENTATION is OFF so the
# Doxygen HTML documentation build (and its Doxygen tool dependency) stays out of the prefix;
# VMA_BUILD_SAMPLES is OFF so the sample application is not built. No Bloom translation unit defines
# VMA_IMPLEMENTATION in this slice, so nothing from the implementation is compiled here.
#
# No in-recipe fetch: the tag archive carries no submodule content and the build phase runs offline.
set(BLOOM_VULKAN_MEMORY_ALLOCATOR_VERSION 3.4.0)
set(BLOOM_VULKAN_MEMORY_ALLOCATOR_URL
    "https://codeload.github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/tar.gz/refs/tags/v3.4.0")
set(BLOOM_VULKAN_MEMORY_ALLOCATOR_SHA256
    822aa850c6ce77346ae96a8a1d351d52e77e85929f35363849a0a4e638e0a2a1)

ExternalProject_Add(bloom_dependency_vulkan-memory-allocator
    URL "${BLOOM_VULKAN_MEMORY_ALLOCATOR_URL}"
    URL_HASH SHA256=${BLOOM_VULKAN_MEMORY_ALLOCATOR_SHA256}
    DOWNLOAD_DIR "${BLOOM_DEPENDENCY_DOWNLOAD_DIR}"
    DOWNLOAD_NAME vulkan-memory-allocator-3.4.0.tar.gz
    DOWNLOAD_NO_PROGRESS ON
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    CMAKE_ARGS
        ${BLOOM_DEPENDENCY_COMMON_CMAKE_ARGS}
        -DVMA_ENABLE_INSTALL:BOOL=ON
        -DVMA_BUILD_DOCUMENTATION:BOOL=OFF
        -DVMA_BUILD_SAMPLES:BOOL=OFF
    BUILD_ALWAYS OFF
    INSTALL_DIR "${BLOOM_DEPENDENCY_PREFIX}"
    LIST_SEPARATOR |
    USES_TERMINAL_DOWNLOAD ON
    USES_TERMINAL_BUILD ON)
