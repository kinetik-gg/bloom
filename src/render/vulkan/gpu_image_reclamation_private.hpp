#ifndef BLOOM_RENDER_VULKAN_GPU_IMAGE_RECLAMATION_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_IMAGE_RECLAMATION_PRIVATE_HPP

#include "gpu_device_private.hpp"
#include <cstddef>

namespace bloom::render {
struct GpuImageImpl;
struct GpuDisplayImageImpl;
namespace image_detail {
inline constexpr std::size_t kImageOwnerCapacity = 4096;
[[nodiscard]] bool reserveImage(GpuImageImpl& image) noexcept;
[[nodiscard]] bool reserveImage(GpuDisplayImageImpl& image) noexcept;
void registerImageBytes(GpuImageImpl& image, std::uint64_t bytes) noexcept;
void registerImageBytes(GpuDisplayImageImpl& image, std::uint64_t bytes) noexcept;
void releaseImageSlot(GpuImageImpl& image) noexcept;
void releaseImageSlot(GpuDisplayImageImpl& image) noexcept;
void orphanImage(GpuImageImpl& image) noexcept;
void orphanImage(GpuDisplayImageImpl& image) noexcept;
void drainImages(vulkan_detail::DeviceAllocatorState& state) noexcept;
void closeImageOwner(vulkan_detail::DeviceAllocatorState& state) noexcept;
[[nodiscard]] GpuImageReclamationStats imageStats(const void* state) noexcept;
// Private native regression seam. Zero restores production capacity; existing slots remain owned.
void setImageDestroyPauseForTests(bool pause) noexcept;
[[nodiscard]] bool imageDestroyPausedForTests() noexcept;
void setImageAdmissionLimitForTests(std::size_t limit) noexcept;
} // namespace image_detail
} // namespace bloom::render
#endif
