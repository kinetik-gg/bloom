#ifndef BLOOM_RENDER_VULKAN_GPU_DEVICE_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_DEVICE_PRIVATE_HPP

// Private to src/render/vulkan. Never included by the public render surface, the CPU stub, or the
// portable test smoke. Vulkan-Hpp supplies typed handles, structs, and RAII owners; Bloom opens the
// platform loader privately at runtime and defines VMA's implementation in its own translation
// unit, so no translation unit needs static Vulkan prototypes.

#if !defined(VK_NO_PROTOTYPES)
#define VK_NO_PROTOTYPES 1
#endif
#if !defined(VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL)
#define VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL 0
#endif
#if !defined(VULKAN_HPP_NO_EXCEPTIONS)
#define VULKAN_HPP_NO_EXCEPTIONS 1
#endif

#if !defined(VMA_STATIC_VULKAN_FUNCTIONS)
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#endif
#if !defined(VMA_DYNAMIC_VULKAN_FUNCTIONS)
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#endif

#include <vulkan/vulkan_raii.hpp>

#include <vk_mem_alloc.h>

#include <cstdint>
#include <memory>
#include <thread>

namespace bloom::render::vulkan_detail {

// Owns one runtime-opened platform loader. The loader must outlive every function pointer,
// instance, and device resolved through it, so it is destroyed after the Vulkan handles below.
class DynamicLibrary final {
  public:
    DynamicLibrary() noexcept = default;
    explicit DynamicLibrary(const char* path) noexcept;
    ~DynamicLibrary();

    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

    [[nodiscard]] void* symbol(const char* name) const noexcept;
    [[nodiscard]] bool isOpen() const noexcept { return handle_ != nullptr; }

  private:
    void close() noexcept;

    void* handle_ = nullptr;
};

// Owns one logical device generation and its memory allocator. Destruction tears the allocator
// down in the body, then lets the RAII members release in reverse declaration order: queue, device,
// physical device, instance, and finally the loader. The owner thread is recorded so misuse can
// fail closed rather than race a device.
struct DeviceAllocatorState final {
    DeviceAllocatorState() = default;
    DeviceAllocatorState(const DeviceAllocatorState&) = delete;
    DeviceAllocatorState& operator=(const DeviceAllocatorState&) = delete;
    DeviceAllocatorState(DeviceAllocatorState&&) = delete;
    DeviceAllocatorState& operator=(DeviceAllocatorState&&) = delete;
    ~DeviceAllocatorState();

    std::thread::id owner;

    DynamicLibrary loader;
    vk::raii::Instance instance{nullptr};
    vk::raii::PhysicalDevice physicalDevice{nullptr};
    vk::raii::Device device{nullptr};
    vk::raii::Queue computeQueue{nullptr};
    VmaAllocator allocator = VK_NULL_HANDLE;
    std::uint32_t computeQueueFamily = 0;

    // Bounded bootstrap facts recorded once so a renderer can validate a request against real
    // device limits before touching the allocator or queue.
    std::uint32_t generation = 0;
    std::uint64_t maxStorageBufferRange = 0;
    std::uint32_t maxComputeWorkGroupCountX = 0;
    std::uint32_t maxComputeWorkGroupInvocations = 0;
    std::uint32_t maxComputeWorkGroupSizeX = 0;
};

} // namespace bloom::render::vulkan_detail

namespace bloom::render {

// The only non-public bridge from a render operation to the device's Vulkan allocator and queue.
// GpuDevice befriends this class; the CPU stub never defines or references it, so a build without
// Vulkan dependencies links the stub API unchanged.
class GpuRendererAccess final {
  public:
    [[nodiscard]] static std::shared_ptr<vulkan_detail::DeviceAllocatorState>
    state(GpuDevice& device) noexcept;
    [[nodiscard]] static std::thread::id owner(const GpuDevice& device) noexcept;
};

} // namespace bloom::render

#endif // BLOOM_RENDER_VULKAN_GPU_DEVICE_PRIVATE_HPP
