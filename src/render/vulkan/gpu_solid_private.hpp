#ifndef BLOOM_RENDER_VULKAN_GPU_SOLID_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_SOLID_PRIVATE_HPP

// Private to src/render/vulkan. Holds the shared GpuSolid impl behind both the
// ordinary SolidV1 path (gpu_solid.cpp) and the CoveredSolidV1 path
// (gpu_solid_covered.cpp). Splitting the impl out keeps each translation unit
// under the 700-line budget while the two operations share one device pipeline
// owner, one command buffer/fence, and one teardown/quarantine policy; no second
// backend class is introduced.

#include <bloom/render/gpu_solid.hpp>

#include "gpu_device_private.hpp"
#include "gpu_image_private.hpp"
#include "gpu_solid_fault.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace bloom::render {

// Shared diagnostic constructor, defined in gpu_solid.cpp, so both the ordinary
// and covered translation units build the same typed diagnostic values.
[[nodiscard]] GpuSolidDiagnostic gpuSolidDiagnostic(GpuSolidDiagnosticCode code,
                                                    std::string message);

// One host-visible storage buffer owned by a covered job. It is written and
// flushed before the dispatch is submitted and retained until the producing
// fence is proved retired, so it can never be freed while a submission still
// references it. The same release-retained policy as the upload staging buffer.
struct GpuSolidUploadBuffer final {
    GpuSolidUploadBuffer() = default;
    GpuSolidUploadBuffer(const GpuSolidUploadBuffer&) = delete;
    GpuSolidUploadBuffer& operator=(const GpuSolidUploadBuffer&) = delete;
    GpuSolidUploadBuffer(GpuSolidUploadBuffer&& other) noexcept { *this = std::move(other); }
    GpuSolidUploadBuffer& operator=(GpuSolidUploadBuffer&& other) noexcept {
        if (this != &other) {
            release();
            state = other.state;
            buffer = other.buffer;
            allocation = other.allocation;
            bytes = other.bytes;
            armed = other.armed;
            owner = std::move(other.owner);
            other.state = nullptr;
            other.buffer = VK_NULL_HANDLE;
            other.allocation = VK_NULL_HANDLE;
            other.bytes = 0;
            other.armed = false;
        }
        return *this;
    }
    ~GpuSolidUploadBuffer() { release(); }

    void release() noexcept {
        if (armed && state != nullptr) {
            vmaDestroyBuffer(state->allocator, buffer, allocation);
        }
        state = nullptr;
        buffer = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
        bytes = 0;
        armed = false;
        owner.reset();
    }

    vulkan_detail::DeviceAllocatorState* state = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    std::uint64_t bytes = 0;
    bool armed = false;
    // For the resident-coverage path this co-owns the producer's mask buffer so
    // it can never be freed while this submission still references it. Null for
    // the ordinary host-uploaded coverage.
    std::shared_ptr<void> owner;
};

// CoveredSolidV1 pipeline resources. Created lazily on the first beginCovered on
// the owner thread so ordinary SolidV1 creation behavior is byte-for-byte
// unchanged; cached for the device generation afterwards. `attempted` makes a
// shader/descriptor failure terminal for this generation instead of retrying.
struct GpuSolidCoveredResources final {
    vk::raii::ShaderModule shaderModule{nullptr};
    vk::raii::DescriptorSetLayout descriptorSetLayout{nullptr};
    vk::raii::PipelineLayout pipelineLayout{nullptr};
    vk::raii::Pipeline pipeline{nullptr};
    vk::raii::DescriptorPool descriptorPool{nullptr};
    vk::raii::DescriptorSet descriptorSet{nullptr};
    bool attempted = false;
    bool ready = false;
    std::string reason;
};

// Sentinel for an Impl that owns no bounded resident-pool slot.
inline constexpr std::size_t kSolidNoResidentSlot = static_cast<std::size_t>(-1);

struct GpuSolid::Impl final {
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl();

    [[nodiscard]] bool onOwnerThread() const noexcept {
        return control != nullptr && control->owner == std::this_thread::get_id();
    }
    void fail(const GpuSolidDiagnosticCode code, std::string message) {
        jobState = GpuSolidJobState::Failure;
        jobDiagnostic = gpuSolidDiagnostic(code, std::move(message));
    }
    void clearJob() {
        jobState = GpuSolidJobState::Idle;
        jobDiagnostic = GpuSolidDiagnostic{};
        discardRequested.store(false);
        residentImage.reset();
        coveredPalette.release();
        coveredMask.release();
    }
    void releaseResident() { residentImage.reset(); }
    // Bounded resident-slot management, defined in gpu_solid_retirement.cpp. Acquire is called
    // before the first native allocation; release is owner-thread retirement; orphan is the
    // foreign-thread or unproven owner path that preserves the already-owned slot for the owner
    // drain. The drain is a static member because it names the private Impl type.
    [[nodiscard]] bool acquireResidentSlot() noexcept;
    void releaseResidentSlot() noexcept;
    void orphanResidentSlot() noexcept;
    static void drainResidentOrphansOnOwnerThread() noexcept;
    // Frees every base pipeline/command/fence native resource. Called only on the owner thread after
    // a failed createPipeline() so an Impl that then holds no resident slot owns no Vulkan object
    // and may be destroyed from any thread.
    void resetPipelineResources() noexcept;
    [[nodiscard]] bool createPipeline();
    // Builds the CoveredSolidV1 shader module, descriptor layout, pipeline
    // layout, pipeline, descriptor pool, and descriptor set.
    [[nodiscard]] bool createCoveredPipeline();
    // Shared implementation of the covered fill. `hostCoverage` is non-empty for
    // the ordinary path and the CPU float mask is uploaded; otherwise the
    // already-resident `residentMaskBuffer` (owned by `residentOwner`) is bound
    // directly with no host roundtrip.
    [[nodiscard]] GpuSolidDiagnostic
    beginCoveredJob(const GpuSolidParameters& base, std::span<const std::uint8_t> hostCoverage,
                    float opacity, std::uint64_t byteBudget, VkBuffer residentMaskBuffer,
                    std::uint64_t residentMaskBytes, std::shared_ptr<void> residentOwner);
    // Bounded owner-thread drain. Returns true when the submission is proved
    // retired.
    [[nodiscard]] bool drainAndRetire() noexcept;

    std::thread::id owner;
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    GpuSolidBudgets budgets;
    std::uint32_t expectedGeneration = 0;
    // Bounded resident-pool slot owned by this Impl from before its first native allocation until
    // owner-thread release. kSolidNoResidentSlot means this Impl owns no native resources.
    std::size_t residentSlot = kSolidNoResidentSlot;
    bool pipelineReady = false;

    vk::raii::ShaderModule shaderModule{nullptr};
    vk::raii::DescriptorSetLayout descriptorSetLayout{nullptr};
    vk::raii::PipelineLayout pipelineLayout{nullptr};
    vk::raii::Pipeline pipeline{nullptr};
    vk::raii::DescriptorPool descriptorPool{nullptr};
    vk::raii::DescriptorSet descriptorSet{nullptr};
    vk::raii::CommandPool commandPool{nullptr};
    vk::raii::CommandBuffer commandBuffer{nullptr};
    vk::raii::Fence fence{nullptr};

    // CoveredSolidV1 resources, created lazily.
    GpuSolidCoveredResources covered;
    // Per-job covered inputs, retained until the submission is proved retired.
    GpuSolidUploadBuffer coveredPalette;
    GpuSolidUploadBuffer coveredMask;

    std::unique_ptr<GpuImage> residentImage;

    GpuSolidJobState jobState = GpuSolidJobState::Idle;
    bool queueSubmitted = false;
    std::uint64_t lastJobBytes = 0;
    bool deviceLost = false;
    std::atomic<bool> discardRequested{false};
    GpuSolidDiagnostic jobDiagnostic;
    GpuSolidDiagnostic createDiagnostic;
};

} // namespace bloom::render

#endif // BLOOM_RENDER_VULKAN_GPU_SOLID_PRIVATE_HPP
