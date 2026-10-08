#include "gpu_image_private.hpp"
#include "gpu_oracle_readback_private.hpp"
#include "gpu_resident_display_private.hpp"

#include <array>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

namespace bloom::render::oracle_detail {
namespace {
struct Slot final {
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> state;
    GpuImageImpl* process = nullptr;
    GpuDisplayImageImpl* display = nullptr;
    std::size_t sourceSlot = 0;
    std::uint64_t token = 0;
    std::uint64_t bytes = 0;
    bool retained = false;
    StagingBuffer staging;
    vk::raii::CommandPool pool{nullptr};
    vk::raii::CommandBuffer buffer{nullptr};
    vk::raii::Fence fence{nullptr};
};
struct Store final {
    std::mutex mutex;
    std::array<Slot, kCapacity> slots;
    std::uint64_t nextToken = 0;
    std::uint64_t retired = 0;
};
Store& store() noexcept {
    // Process-exit teardown cannot establish completion for an exited owner.
    alignas(Store) static std::byte storage[sizeof(Store)];
    static auto* const instance = ::new (static_cast<void*>(storage)) Store;
    return *instance;
}
thread_local bool forceTimeout = false;
thread_local bool deferRetirement = false;
thread_local bool refuseAdmission = false;
bool owner(const vulkan_detail::DeviceAllocatorState& state) noexcept {
    return state.owner == std::this_thread::get_id() && state.ownerLifetimeToken != 0 &&
           state.ownerLifetimeToken == vulkan_detail::deviceOwnerLifetimeToken();
}
template <typename Image>
void reserve(Image& image, std::size_t& index, std::uint64_t& token) noexcept {
    if (!image.onOwnerThread() || image.state->imageOwnerClosed || refuseAdmission ||
        image.submissionUnretired.load(std::memory_order_acquire)) {
        return;
    }
    auto& storage = store();
    std::lock_guard lock(storage.mutex);
    for (const auto& slot : storage.slots) {
        if (slot.state == image.state) {
            return;
        }
    }
    // Never wrap an identity, even at a practically unreachable token exhaustion.
    if (storage.nextToken == UINT64_MAX) {
        return;
    }
    for (std::size_t candidate = 0; candidate < kCapacity; ++candidate) {
        auto& slot = storage.slots[candidate];
        if (slot.state != nullptr) {
            continue;
        }
        slot.state = image.state;
        if constexpr (std::is_same_v<Image, GpuImageImpl>) {
            slot.process = &image;
        } else {
            slot.display = &image;
        }
        slot.sourceSlot = image.reclamationSlot;
        slot.token = ++storage.nextToken;
        index = candidate;
        token = slot.token;
        image.state->pendingOracleReadbacks.fetch_add(1, std::memory_order_release);
        return;
    }
}
void release(Slot& slot) noexcept {
    slot.state->pendingOracleReadbacks.fetch_sub(1, std::memory_order_release);
    slot.process = nullptr;
    slot.display = nullptr;
    slot.bytes = 0;
    slot.retained = false;
    slot.token = 0;
    slot.state.reset();
}
template <typename Image> bool exact(const Slot& slot, const Image* image) noexcept {
    return image != nullptr && image->state == slot.state && image->onOwnerThread() &&
           image->reclamationSlot == slot.sourceSlot && image->oracleReadbackToken == slot.token &&
           image->submissionUnretired.load(std::memory_order_acquire);
}
} // namespace

StagingBuffer::StagingBuffer(StagingBuffer&& other) noexcept { *this = std::move(other); }
StagingBuffer& StagingBuffer::operator=(StagingBuffer&& other) noexcept {
    if (this != &other) {
        release();
        state = std::exchange(other.state, nullptr);
        buffer = std::exchange(other.buffer, VK_NULL_HANDLE);
        allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
        armed = std::exchange(other.armed, false);
    }
    return *this;
}
StagingBuffer::~StagingBuffer() { release(); }
void StagingBuffer::release() noexcept {
    if (armed && state != nullptr) {
        vmaDestroyBuffer(state->allocator, buffer, allocation);
    }
    state = nullptr;
    buffer = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    armed = false;
}
Reservation::Reservation(GpuImageImpl& source) noexcept { reserve(source, index_, token_); }
Reservation::Reservation(GpuDisplayImageImpl& source) noexcept { reserve(source, index_, token_); }
Reservation::~Reservation() {
    if (!valid()) {
        return;
    }
    auto& storage = store();
    std::lock_guard lock(storage.mutex);
    auto& slot = storage.slots[index_];
    if (slot.token == token_ && !slot.retained) {
        release(slot);
    }
}
void Reservation::charge(std::uint64_t bytes) noexcept {
    auto& storage = store();
    std::lock_guard lock(storage.mutex);
    if (valid() && storage.slots[index_].token == token_) {
        storage.slots[index_].bytes = bytes;
    }
}
void Reservation::retain(StagingBuffer&& staging, vk::raii::CommandPool&& pool,
                         vk::raii::CommandBuffer&& buffer, vk::raii::Fence&& fence) noexcept {
    auto& storage = store();
    std::lock_guard lock(storage.mutex);
    auto& slot = storage.slots[index_];
    slot.staging = std::move(staging);
    slot.pool = std::move(pool);
    slot.buffer = std::move(buffer);
    slot.fence = std::move(fence);
    if (slot.process != nullptr) {
        slot.process->oracleReadbackToken = slot.token;
        slot.process->submissionUnretired.store(true, std::memory_order_release);
    } else {
        slot.display->oracleReadbackToken = slot.token;
        slot.display->submissionUnretired.store(true, std::memory_order_release);
    }
    slot.retained = true;
}
void drain(vulkan_detail::DeviceAllocatorState& state) noexcept {
    if (!owner(state) || state.imageOwnerClosed ||
        state.pendingOracleReadbacks.load(std::memory_order_acquire) == 0 || deferRetirement) {
        return;
    }
    auto& storage = store();
    std::lock_guard lock(storage.mutex);
    for (auto& slot : storage.slots) {
        if (slot.state.get() != &state || !slot.retained ||
            (!exact(slot, slot.process) && !exact(slot, slot.display))) {
            continue;
        }
        const VkResult result = state.device.getDispatcher()->vkGetFenceStatus(
            static_cast<VkDevice>(*state.device), static_cast<VkFence>(*slot.fence));
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
            continue;
        }
        slot.buffer = vk::raii::CommandBuffer{nullptr};
        slot.pool = vk::raii::CommandPool{nullptr};
        slot.fence = vk::raii::Fence{nullptr};
        slot.staging.release();
        if (slot.process != nullptr) {
            slot.process->deviceLost = result == VK_ERROR_DEVICE_LOST;
            slot.process->oracleReadbackToken = 0;
            slot.process->submissionUnretired.store(false, std::memory_order_release);
        } else {
            slot.display->deviceLost = result == VK_ERROR_DEVICE_LOST;
            slot.display->oracleReadbackToken = 0;
            slot.display->submissionUnretired.store(false, std::memory_order_release);
        }
        // The source may now be reclaimed; never dereference it after clearing the pin.
        release(slot);
        ++storage.retired;
    }
}
VkResult wait(vulkan_detail::DeviceAllocatorState& state, VkFence fence,
              std::uint64_t deadline) noexcept {
    if (forceTimeout) {
        forceTimeout = false;
        return VK_TIMEOUT;
    }
    return state.device.getDispatcher()->vkWaitForFences(static_cast<VkDevice>(*state.device), 1,
                                                         &fence, VK_TRUE, deadline);
}
Stats stats(const void* state) noexcept {
    auto& storage = store();
    std::lock_guard lock(storage.mutex);
    Stats result;
    result.retired = storage.retired;
    for (const auto& slot : storage.slots) {
        if (slot.state == nullptr || (state != nullptr && slot.state.get() != state)) {
            continue;
        }
        ++result.reservations;
        result.quarantined += slot.retained ? 1U : 0U;
        result.stagingBytes += slot.bytes;
    }
    return result;
}
void setTimeoutForTests(bool timeout) noexcept { forceTimeout = timeout; }
void setRetirementDeferredForTests(bool deferred) noexcept { deferRetirement = deferred; }
void setAdmissionRefusedForTests(bool refused) noexcept { refuseAdmission = refused; }
} // namespace bloom::render::oracle_detail
