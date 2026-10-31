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
#include "gpu_image_upload_retirement_private.hpp"
#include "gpu_native_buffer_allocation_private.hpp"

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
// #401: the transient RGBA16F image a half-exact staging buffer is copied into before one exact
// half-to-float blit into the resident RGBA32F image. Owned by the staging buffer it belongs to and
// released with it, so every existing staging release path (fence proof, failure, quarantine) also
// retires it.
struct UploadHalfImage final {
    UploadHalfImage() = default;
    UploadHalfImage(const UploadHalfImage&) = delete;
    UploadHalfImage& operator=(const UploadHalfImage&) = delete;
    UploadHalfImage(UploadHalfImage&& other) noexcept { *this = std::move(other); }
    UploadHalfImage& operator=(UploadHalfImage&& other) noexcept;
    ~UploadHalfImage() { release(); }

    void release() noexcept;

    vulkan_detail::DeviceAllocatorState* state = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    NativeImageStorageToken token;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// True when the device can take the half path: the upload queue family has graphics capability
// (vkCmdBlitImage needs it) and RGBA16F optimal images support TRANSFER_DST and BLIT_SRC while the
// resident RGBA32F format supports BLIT_DST.
[[nodiscard]] bool uploadHalfStagingSupported(vulkan_detail::DeviceAllocatorState& state) noexcept;

// Creates the transient RGBA16F image (TRANSFER_DST | TRANSFER_SRC) under the current phase.
[[nodiscard]] bool createUploadHalfImage(vulkan_detail::DeviceAllocatorState& state,
                                         std::uint32_t width, std::uint32_t height,
                                         UploadHalfImage& out) noexcept;

// Writes the IEEE binary16 encoding of `count` floats when every one of them is exactly
// representable (zero, infinities, half normals and subnormals; never NaN or a float subnormal).
// Returns false as soon as one is not; `destination` is then unspecified.
[[nodiscard]] bool packExactHalf(const float* source, std::uint16_t* destination,
                                 std::size_t count) noexcept;

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
    NativeBufferStorageToken token;
    // Present only while this buffer holds half-exact pixels for the current job.
    UploadHalfImage half;
};

// Creates a host-visible, mapped, TRANSFER_SRC buffer sized `bytes`. Returns false and leaves `out`
// untouched on failure; backed failures destroy native storage before returning credit.
[[nodiscard]] vulkan_detail::NativeBufferAllocationOutcome
createUploadStagingBuffer(vulkan_detail::DeviceAllocatorState& state, std::uint64_t bytes,
                          std::uint64_t maximumBackingBytes, UploadStagingBuffer& out) noexcept;

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
    static void drainResidentOrphansOnOwnerThread(
        const vulkan_detail::DeviceAllocatorState* exactOwner = nullptr) noexcept;
    // Frees the command pool/buffer/fence. Called only on the owner thread after a failed
    // createResources() so an Impl that then holds no resident slot owns no Vulkan object and may
    // be destroyed from any thread.
    void resetResources() noexcept;
    [[nodiscard]] bool createResources();
    // Bounded owner-thread drain. Returns true when the submission is proved retired.
    [[nodiscard]] bool drainAndRetire() noexcept;

    std::thread::id owner;
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    GpuImageUploadBudgets budgets;
    std::uint64_t expectedEpoch = 0;
    // Bounded resident-pool slot owned by this Impl from before its first native allocation until
    // owner-thread release. kUploadNoResidentSlot means this Impl owns no native resources.
    std::size_t residentSlot = kUploadNoResidentSlot;
    bool resourcesReady = false;

    vk::raii::CommandPool commandPool{nullptr};
    vk::raii::CommandBuffer commandBuffer{nullptr};
    vk::raii::Fence fence{nullptr};

    std::unique_ptr<GpuImage> residentImage;
    UploadStagingBuffer staging;
    // #401: the last proven-retired staging buffer, kept for the next upload of the same scene
    // (same processing phase) instead of allocating a fresh mapped buffer per layer per frame.
    // releaseSpareStaging() frees it when the scene ends; it is never reused across phases.
    UploadStagingBuffer spareStaging;
    const void* sparePhase = nullptr;
    // Whether the current job opted in (GpuImageUploadParameters::reuseStagingWithinScene).
    bool retainSpare = false;
    std::uint64_t stagingAllocations = 0;
    // #401 half staging: -1 unknown, 0 unsupported, 1 supported (queried once per Impl).
    int halfSupport = -1;
    std::uint64_t halfStagedUploads = 0;
    // The last retired transient half image, reused like spareStaging (same phase, extent at
    // least the upload's; copy and blit address only the upload's region).
    UploadHalfImage spareHalf;
    const void* spareHalfPhase = nullptr;

    GpuImageUploadJobState jobState = GpuImageUploadJobState::Idle;
    bool queueSubmitted = false;
    std::uint64_t lastJobBytes = 0;
    bool deviceLost = false;
    std::atomic<bool> discardRequested{false};
    GpuImageUploadDiagnostic jobDiagnostic;
    GpuImageUploadDiagnostic createDiagnostic;
};

struct GpuImageUploadRetirementAccess final {
    static void drain(vulkan_detail::DeviceAllocatorState& state) noexcept {
        GpuImageUpload::Impl::drainResidentOrphansOnOwnerThread(&state);
    }
};

} // namespace bloom::render

#endif // BLOOM_RENDER_VULKAN_GPU_IMAGE_UPLOAD_PRIVATE_HPP
