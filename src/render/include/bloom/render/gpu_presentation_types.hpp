#pragma once

// Bloom-owned, Qt-free and Vulkan-free presentation bootstrap values. They carry no Vk*/native
// type: native handles cross the boundary as opaque integer bits, and the epoch binds every
// borrowed handle to exactly one live GpuDevice instance so a stale UI surface can never touch a
// newer device. Nothing here owns a device or destroys one.

#include <cstdint>
#include <string>

namespace bloom::render {

enum class GpuPresentationPlatform : std::uint8_t {
    None,
    Wayland,
    // Declared for the future XCB/Xlib dependency intake. This slice enables neither: a request for
    // one stays explicitly Unavailable while the compute path is preserved.
    Xcb,
    Xlib,
};

enum class GpuPresentationAvailability : std::uint8_t {
    // The caller did not ask for presentation; the device is exactly the compute-only bootstrap.
    NotRequested,
    // Presentation was requested but at least one required capability is missing. The compute path
    // remains functional and the typed detail says which capability is absent.
    Unavailable,
    // The requested platform's surface extension, VK_KHR_surface, VK_KHR_swapchain, and a
    // present-capable queue were all actually enabled. This makes no claim about any specific
    // surface: surface support is answered only by validateBorrowedSurface().
    Ready,
};

// Unique, monotonic stamp minted once per created GpuDevice instance. A borrowed instance/surface
// carries the epoch of the device it was minted against; a mismatch is rejected before any driver
// call. It is deliberately per-instance, never a process-wide service locator.
struct GpuPresentationEpoch final {
    std::uint64_t value = 0;

    friend bool operator==(const GpuPresentationEpoch&, const GpuPresentationEpoch&) = default;
};

// The exact capability facts behind presentationAvailability(). `detail` explains a non-Ready
// status in one bounded sentence.
struct GpuPresentationStatus final {
    GpuPresentationAvailability availability = GpuPresentationAvailability::NotRequested;
    GpuPresentationPlatform platform = GpuPresentationPlatform::None;
    bool surface_extension = false;
    bool platform_surface_extension = false;
    bool swapchain_extension = false;
    bool present_queue = false;
    // VK_EXT/KHR_swapchain_maintenance1 present fences are enabled and available.
    bool present_fences = false;
    // VK_KHR_present_wait + VK_KHR_present_id are enabled and available.
    bool present_wait = false;
    std::string detail;

    friend bool operator==(const GpuPresentationStatus&, const GpuPresentationStatus&) = default;
};

// Non-owning borrowed view of the live instance, for a UI-side QVulkanInstance::setVkInstance().
// `instance_bits` is the VkInstance reinterpreted as an integer and `present_queue_family` is the
// queue family a swapchain must be created on. Immutable after create(), so it is safe to read from
// the UI thread. The view owns nothing: the caller must keep the GpuDevice alive (on its owner
// thread) until every borrowed surface has been retired and acknowledged.
struct GpuBorrowedInstanceView final {
    bool valid = false;
    std::uint64_t instance_bits = 0;
    std::uint32_t present_queue_family = 0;
    GpuPresentationEpoch epoch;
};

// Opaque borrowed surface the UI minted against a borrowed instance view: `surface_bits` is the
// VkSurfaceKHR reinterpreted as an integer plus the epoch it was created under. Non-owning.
struct GpuBorrowedSurface final {
    std::uint64_t surface_bits = 0;
    GpuPresentationEpoch epoch;
};

enum class GpuSurfaceSupport : std::uint8_t {
    Supported,
    // The surface is real but the preselected present queue family cannot present to it.
    UnsupportedByQueue,
    // Presentation is not Ready for this device generation.
    PresentationUnavailable,
    // The borrowed epoch does not match the live device; rejected before any driver call.
    WrongEpoch,
    // The query was attempted from a non-owner thread; rejected before any driver call.
    WrongThread,
    // A zero surface handle was passed; rejected before any driver call.
    InvalidArgument,
    // The driver rejected the query or the entry point is absent.
    DriverUnavailable,
};

struct GpuSurfaceSupportResult final {
    GpuSurfaceSupport status = GpuSurfaceSupport::PresentationUnavailable;
    std::string message;

    friend bool operator==(const GpuSurfaceSupportResult&,
                           const GpuSurfaceSupportResult&) = default;
};

} // namespace bloom::render
