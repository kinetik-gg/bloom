#ifndef BLOOM_RENDER_VULKAN_GPU_IMAGE_UPLOAD_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_IMAGE_UPLOAD_PRIVATE_HPP

// Private to src/render/vulkan. Defines the opaque impl behind bloom::render::GpuImageUpload and
// the owned staging-buffer helper both translation units share.
//
// This file consumes, but does not mutate, the accepted resident-image substrate:
// GpuImageImpl/createResidentImage/querySolidImageSupport from gpu_image_private.hpp and
// GpuRendererAccess/DeviceAllocatorState from gpu_device_private.hpp. It introduces NO new GpuImage
// accessor: the single read-only seam for consumers remains gpuImageImpl(const GpuImage&) from the
// resident-display package, and the competing accessGpuImageImpl name from the composite package
// must be replaced by it rather than extended here. Upload only constructs images it owns.

#include <bloom/render/gpu_image_upload.hpp>

#include "gpu_device_private.hpp"
#include "gpu_image_private.hpp"
#include "gpu_image_upload_fault.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace bloom::render {

// One owned host-visible staging buffer holding the copied source bytes. Co-owns the device
// allocator generation; owner-thread destruction only. Moved into the bounded quarantine on an
// unproved retirement instead of being destroyed.
struct UploadStagingBuffer final {
    UploadStagingBuffer() = default;
    UploadStagingBuffer(const UploadStagingBuffer&) = delete;
    UploadStagingBuffer& operator=(const UploadStagingBuffer&) = delete;
    UploadStagingBuffer(UploadStagingBuffer&& other) noexcept { *this = std::move(other); }
    UploadStagingBuffer& operator=(UploadStagingBuffer&& other) noexcept;
    ~UploadStagingBuffer();

    void release() noexcept;

    // Borrowed: the owning Impl/quarantine holds the allocator generation shared_ptr beside it.
    vulkan_detail::DeviceAllocatorState* state = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    std::uint64_t bytes = 0;
    bool armed = false;
};

// Creates a host-visible, mapped, TRANSFER_SRC buffer sized `bytes`. Returns false and leaves `out`
// untouched on failure; no allocation happens on the failure path.
[[nodiscard]] bool createUploadStagingBuffer(vulkan_detail::DeviceAllocatorState& state,
                                             std::uint64_t bytes,
                                             UploadStagingBuffer& out) noexcept;

// Actual VMA allocation size in bytes, or 0 when the allocation is invalid. This is the number the
// peak budget is charged against, not the requested extent.
[[nodiscard]] std::uint64_t uploadAllocationBytes(vulkan_detail::DeviceAllocatorState& state,
                                                  VmaAllocation allocation) noexcept;

[[nodiscard]] inline GpuImageUploadDiagnostic
uploadDiagnostic(const GpuImageUploadDiagnosticCode code, std::string message) {
    return GpuImageUploadDiagnostic{code, std::move(message)};
}

// Sentinel for an Impl that owns no bounded resident-pool slot.
inline constexpr std::size_t kUploadNoResidentSlot = static_cast<std::size_t>(-1);

// The single job state machine behind GpuImageUpload, defined here so both translation units can
// implement its members without a second public header or a second Impl definition.
struct GpuImageUpload::Impl final {
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl();

    [[nodiscard]] bool onOwnerThread() const noexcept {
        return control != nullptr && control->owner == std::this_thread::get_id();
    }
    void fail(GpuImageUploadDiagnosticCode code, std::string message) {
        jobState = GpuImageUploadJobState::Failure;
        jobDiagnostic = uploadDiagnostic(code, std::move(message));
    }
    void clearJob() {
        jobState = GpuImageUploadJobState::Idle;
        jobDiagnostic = GpuImageUploadDiagnostic{};
        discardRequested.store(false);
        residentImage.reset();
        staging.release();
    }
    void releaseResident() { residentImage.reset(); }
    // Bounded resident-slot management, defined in gpu_image_upload_retirement.cpp. Acquire is
    // called before the first native allocation; release is owner-thread retirement; orphan is the
    // foreign-thread or unproven owner path that preserves the already-owned slot for the owner
    // drain. The drain is a static member because it names the private Impl type.
    [[nodiscard]] bool acquireResidentSlot() noexcept;
    void releaseResidentSlot() noexcept;
    void orphanResidentSlot() noexcept;
    static void drainResidentOrphansOnOwnerThread() noexcept;
    // Frees the command pool/buffer/fence. Called only on the owner thread after a failed
    // createResources() so an Impl that then holds no resident slot owns no Vulkan object and may be
    // destroyed from any thread.
    void resetResources() noexcept;
    [[nodiscard]] bool createResources();
    // Bounded owner-thread drain. Returns true when the submission is proved retired.
    [[nodiscard]] bool drainAndRetire() noexcept;

    std::thread::id owner;
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    GpuImageUploadBudgets budgets;
    std::uint32_t expectedGeneration = 0;
    // Bounded resident-pool slot owned by this Impl from before its first native allocation until
    // owner-thread release. kUploadNoResidentSlot means this Impl owns no native resources.
    std::size_t residentSlot = kUploadNoResidentSlot;
    bool resourcesReady = false;

    vk::raii::CommandPool commandPool{nullptr};
    vk::raii::CommandBuffer commandBuffer{nullptr};
    vk::raii::Fence fence{nullptr};

    std::unique_ptr<GpuImage> residentImage;
    UploadStagingBuffer staging;

    GpuImageUploadJobState jobState = GpuImageUploadJobState::Idle;
    bool queueSubmitted = false;
    std::uint64_t lastJobBytes = 0;
    bool deviceLost = false;
    std::atomic<bool> discardRequested{false};
    GpuImageUploadDiagnostic jobDiagnostic;
    GpuImageUploadDiagnostic createDiagnostic;
};

} // namespace bloom::render

#endif // BLOOM_RENDER_VULKAN_GPU_IMAGE_UPLOAD_PRIVATE_HPP
