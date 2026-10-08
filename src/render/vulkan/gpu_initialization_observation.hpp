#ifndef BLOOM_RENDER_VULKAN_GPU_INITIALIZATION_OBSERVATION_HPP
#define BLOOM_RENDER_VULKAN_GPU_INITIALIZATION_OBSERVATION_HPP

#include <cstdint>

namespace bloom::render {
class GpuDevice;
namespace primitive_detail {
// Owner-thread-only snapshot of live VMA allocations, excluding reusable committed blocks.
struct InitializationAllocationStats final {
    std::uint64_t count = 0;
    std::uint64_t bytes = 0;
    friend bool operator==(const InitializationAllocationStats&,
                           const InitializationAllocationStats&) = default;
};
[[nodiscard]] InitializationAllocationStats initializationAllocationStats(GpuDevice& device);
} // namespace primitive_detail
} // namespace bloom::render
#endif
