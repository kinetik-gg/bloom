#include "gpu_native_buffer_allocation_private.hpp"

#include <bloom/render/gpu_allocator_memory.hpp>

#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
using namespace bloom::render;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void actual(GpuDevice& device) {
    const auto snapshot = device.allocatorMemorySnapshot();
    const auto control = GpuRendererAccess::state(device);
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(control->allocator, &stats);
    require(snapshot.available &&
                snapshot.allocationBytes == stats.total.statistics.allocationBytes &&
                snapshot.allocationCount == stats.total.statistics.allocationCount &&
                snapshot.blockBytes == stats.total.statistics.blockBytes &&
                snapshot.blockCount == stats.total.statistics.blockCount &&
                snapshot.blockBytes >= snapshot.allocationBytes,
            "owner snapshot exactly separates actual VMA allocation payload and retained blocks");
    require((snapshot.driverBudgetStatus == GpuDriverMemoryBudgetStatus::Available) ==
                control->memoryBudgetEnabled,
            "only an actually enabled live-budget extension supplies driver heap evidence");
}
struct OwnedBuffer final {
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    NativeBufferStorageToken token;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    ~OwnedBuffer() {
        if (allocation != VK_NULL_HANDLE)
            vmaDestroyBuffer(control->allocator, buffer, allocation);
        // Member token destruction follows the body, after actual backing destruction.
    }
};
void run(GpuDevice& device) {
    constexpr std::uint64_t kCapacity = 16ULL * 1024 * 1024;
    GpuProcessingPhaseBudget root;
    root.setHostCapacity(kCapacity, true);
    root.setDeviceCapacity(kCapacity, true);
    require(device.bindProcessingRoot(root.rootIdentity()),
            "test device binds owned allocation root");
    const auto control = GpuRendererAccess::state(device);
    const auto baseline = device.allocatorMemorySnapshot();
    actual(device);
    {
        OwnedBuffer owned;
        owned.control = control;
        const VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                      nullptr,
                                      0,
                                      4096,
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                      VK_SHARING_MODE_EXCLUSIVE,
                                      0,
                                      nullptr};
        VmaAllocationCreateInfo preferences{};
        preferences.usage = VMA_MEMORY_USAGE_AUTO;
        preferences.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        VmaAllocationInfo allocated{};
        require(vulkan_detail::allocateCoveredBuffer(
                    *control, info, preferences, kCapacity, owned.buffer, owned.allocation,
                    allocated, owned.token, vulkan_detail::NativeBufferPurpose::Presentation) ==
                    vulkan_detail::NativeBufferAllocationOutcome::Allocated,
                "real rooted owned native allocation succeeds");
        const auto live = device.allocatorMemorySnapshot();
        require(live.allocationCount == baseline.allocationCount + 1 &&
                    live.allocationBytes == baseline.allocationBytes + allocated.size &&
                    root.snapshot().ownedNativeBufferBytes == allocated.size,
                "actual native backing, precise token and allocator payload agree");
        actual(device);
        GpuAllocatorMemorySnapshot foreign;
        std::thread worker([&] { foreign = device.allocatorMemorySnapshot(); });
        worker.join();
        require(!foreign.available &&
                    foreign.driverBudgetStatus == GpuDriverMemoryBudgetStatus::Unknown,
                "foreign consumers get unavailable instead of performing driver or allocator work");
        const bool enabled = control->memoryBudgetEnabled;
        control->memoryBudgetEnabled = false;
        const auto unavailable = device.allocatorMemorySnapshot();
        control->memoryBudgetEnabled = enabled;
        require(unavailable.available && unavailable.allocationBytes == live.allocationBytes &&
                    unavailable.driverBudgetStatus == GpuDriverMemoryBudgetStatus::Unknown &&
                    unavailable.deviceLocalHeapCount == 0,
                "missing enabled budget capability retains actual VMA stats with explicit unknown "
                "driver usage instead of estimates");
    }
    const auto settled = device.allocatorMemorySnapshot();
    actual(device);
    require(settled.allocationBytes == baseline.allocationBytes &&
                settled.allocationCount == baseline.allocationCount &&
                root.snapshot().ownedNativeBufferBytes == 0 &&
                root.snapshot().totalDeviceClaimBytes == 0,
            "backing destruction returns payload and root claims while reporting retained blocks "
            "independently");
}
} // namespace
int main(int argc, char** argv) {
    try {
        bool requireDevice = false;
        for (int index = 1; index < argc; ++index) {
            if (std::string_view(argv[index]) != "--require-device")
                return 2;
            requireDevice = true;
        }
        auto device = GpuDevice::create();
        if (!device) {
            std::cout << "Unsupported: " << device.diagnostic.message << '\n';
            return requireDevice ? 1 : 77;
        }
        run(*device.device);
        std::cout << "PASS: actual VMA payload/blocks, enabled driver budget, owned retirement\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
