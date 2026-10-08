#pragma once

// Debug/oracle transfers only. Production final readback owns its separate asynchronous job.
#include "gpu_device_private.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>

namespace bloom::render {
struct GpuImageImpl;
struct GpuDisplayImageImpl;
namespace oracle_detail {
inline constexpr std::size_t kCapacity = 2;
struct StagingBuffer final {
    StagingBuffer() = default;
    StagingBuffer(const StagingBuffer&) = delete;
    StagingBuffer& operator=(const StagingBuffer&) = delete;
    StagingBuffer(StagingBuffer&& other) noexcept;
    StagingBuffer& operator=(StagingBuffer&& other) noexcept;
    ~StagingBuffer();
    void release() noexcept;
    vulkan_detail::DeviceAllocatorState* state = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    bool armed = false;
};
// Admission remains held until native destruction, including all pre-submit failures.
class Reservation final {
  public:
    explicit Reservation(GpuImageImpl& source) noexcept;
    explicit Reservation(GpuDisplayImageImpl& source) noexcept;
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    ~Reservation();
    [[nodiscard]] bool valid() const noexcept { return index_ < kCapacity; }
    void charge(std::uint64_t bytes) noexcept;
    void retain(StagingBuffer&& staging, vk::raii::CommandPool&& pool,
                vk::raii::CommandBuffer&& buffer, vk::raii::Fence&& fence) noexcept;

  private:
    std::size_t index_ = kCapacity;
    std::uint64_t token_ = 0;
};
void drain(vulkan_detail::DeviceAllocatorState& state) noexcept;
[[nodiscard]] VkResult wait(vulkan_detail::DeviceAllocatorState& state, VkFence fence,
                            std::uint64_t deadline) noexcept;
struct Stats final {
    std::size_t reservations = 0;
    std::size_t quarantined = 0;
    std::uint64_t stagingBytes = 0;
    std::uint64_t retired = 0;
};
[[nodiscard]] Stats stats(const void* state = nullptr) noexcept;
// Thread-local observation seams: no driver device loss or fence completion is fabricated.
void setTimeoutForTests(bool timeout) noexcept;
void setRetirementDeferredForTests(bool deferred) noexcept;
void setAdmissionRefusedForTests(bool refused) noexcept;
} // namespace oracle_detail
} // namespace bloom::render
