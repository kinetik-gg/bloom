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

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_presentation_types.hpp>

#include <vulkan/vulkan_raii.hpp>

#include <vk_mem_alloc.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>

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
    // Non-owning aliases of the same VkDevice queues (vk::raii::Queue has no destroy). presentQueue
    // is only a distinct queue when the presentation family differs from the compute family.
    vk::raii::Queue presentQueue{nullptr};
    VmaAllocator allocator = VK_NULL_HANDLE;
    std::uint32_t computeQueueFamily = 0;
    std::uint32_t presentQueueFamily = 0;

    // Presentation bootstrap facts. All are set once during create() on the owner thread and are
    // immutable afterwards, so borrowedInstanceView() can read them from the UI thread.
    bool presentationRequested = false;
    bool presentationReady = false;
    bool surfaceExtensionEnabled = false;
    bool platformSurfaceExtensionEnabled = false;
    bool swapchainExtensionEnabled = false;
    bool presentFencesEnabled = false;
    bool presentWaitEnabled = false;
    std::uint64_t presentationEpoch = 0;
    std::uint64_t borrowedInstanceBits = 0;

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

// Native handles cross the public boundary as integer bits only; no Vk* type is ever public.
// Defined here so the bootstrap and resource translation units share one conversion without a new
// header.
template <typename Handle>
[[nodiscard]] inline std::uint64_t handleBits(const Handle handle) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
}

template <typename Handle>
[[nodiscard]] inline Handle handleFromBits(const std::uint64_t bits) noexcept {
    // The public contract carries native handles as opaque integer bits only. Reconstructing the
    // typed handle is the single necessary int-to-pointer boundary and has no safer equivalent.
    // NOLINTNEXTLINE(performance-no-int-to-ptr): documented native-handle reconstruction boundary.
    return reinterpret_cast<Handle>(static_cast<std::uintptr_t>(bits));
}

[[nodiscard]] inline GpuDiagnostic diagnostic(const GpuDiagnosticCode code, std::string message) {
    return GpuDiagnostic{code, std::move(message)};
}

// The single device-generation state behind GpuDevice. Defined here so the bootstrap translation
// unit can construct it during create() while the resource translation unit implements the
// remaining members, with no second definition of the pimpl.
struct GpuDevice::Impl final {
    std::thread::id owner;
    GpuDeviceState state = GpuDeviceState::Unavailable;
    GpuCapabilityReport report;
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;

    // Immutable after create(): safe for borrowedInstanceView() reads from the UI thread.
    bool presentationReady = false;
    std::uint64_t presentationEpoch = 0;
    std::uint32_t presentQueueFamily = 0;
    std::uint64_t borrowedInstanceBits = 0;
};

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
