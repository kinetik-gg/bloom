#ifndef BLOOM_RENDER_VULKAN_GPU_BOUNDED_RETIREMENT_HPP
#define BLOOM_RENDER_VULKAN_GPU_BOUNDED_RETIREMENT_HPP

// Private to src/render/vulkan. A fixed-capacity, allocation-free store of the native-resource-
// owning Impl pointers behind the GpuSolid and GpuImageUpload families. Every Impl that owns native
// resources holds exactly one slot, acquired BEFORE its first native allocation and held until
// owner-thread release. A foreign-thread destruction only marks the already-owned slot orphaned (it
// never destroys native state off-thread), so the retained set can never exceed the fixed capacity
// and a full pool refuses new work cleanly instead of leaking per object. The owner drain retires
// orphaned slots allocation-free and returns them, so pressure is recoverable rather than a
// permanent fuse.
//
// The store is deliberately type-erased and Vulkan-free: it holds opaque Impl pointers and invokes
// caller-supplied callbacks. The exact owner-thread/device-generation gate and the fence-proof
// decision live in the owning translation unit, inside a member of the private nested Impl (the
// only context that may name that type).

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>

namespace bloom::render::primitive_detail {

template <std::size_t Capacity>
class VoidResidentSlotStore final {
  public:
    static constexpr std::size_t capacity = Capacity;

    VoidResidentSlotStore() = default;
    VoidResidentSlotStore(const VoidResidentSlotStore&) = delete;
    VoidResidentSlotStore& operator=(const VoidResidentSlotStore&) = delete;

    // Acquires a free slot for `impl` before any native allocation. Returns false when the bounded
    // pool is full; the caller refuses cleanly without allocating.
    [[nodiscard]] bool acquire(void* const impl, std::size_t& index) noexcept {
        std::lock_guard lock(mutex_);
        for (std::size_t candidate = 0; candidate < Capacity; ++candidate) {
            if (slots_[candidate].impl == nullptr) {
                slots_[candidate] = Slot{impl, false};
                index = candidate;
                return true;
            }
        }
        refusals_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Owner-thread release: returns the slot. Never touches another owner's resources.
    void release(void* const impl, const std::size_t index) noexcept {
        if (impl == nullptr || index >= Capacity) {
            return;
        }
        std::lock_guard lock(mutex_);
        Slot& slot = slots_[index];
        if (slot.impl == impl) {
            slot = Slot{};
        }
    }

    // Foreign-thread destruction: preserve the original ownership in the slot, never destroy native
    // state off-thread.
    void orphan(void* const impl, const std::size_t index) noexcept {
        if (impl == nullptr || index >= Capacity) {
            return;
        }
        std::lock_guard lock(mutex_);
        Slot& slot = slots_[index];
        if (slot.impl == impl) {
            slot.orphaned = true;
        }
    }

    [[nodiscard]] std::size_t inUse() const noexcept {
        std::lock_guard lock(mutex_);
        std::size_t count = 0;
        for (const auto& slot : slots_) {
            count += slot.impl != nullptr ? 1U : 0U;
        }
        return count;
    }

    [[nodiscard]] std::size_t orphaned() const noexcept {
        std::lock_guard lock(mutex_);
        std::size_t count = 0;
        for (const auto& slot : slots_) {
            count += slot.orphaned ? 1U : 0U;
        }
        return count;
    }

    [[nodiscard]] std::uint64_t refusals() const noexcept {
        return refusals_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t retired() const noexcept {
        return retired_.load(std::memory_order_relaxed);
    }

    // Non-blocking owner drain. `owned` and `retire` run under the lock; `destroy` runs outside it,
    // so an Impl destructor never re-enters the store. Ownership is only moved out once `retire` has
    // proven the slot safe to free, so an unproven submission is retained intact. Allocation-free
    // and noexcept: the staging array is fixed-capacity.
    template <typename OwnedFn, typename RetireFn, typename DestroyFn>
    void drainOrphans(OwnedFn&& owned, RetireFn&& retire, DestroyFn&& destroy) noexcept {
        std::array<void*, Capacity> freed{};
        std::size_t freedCount = 0;
        {
            std::lock_guard lock(mutex_);
            for (auto& slot : slots_) {
                if (slot.impl == nullptr || !slot.orphaned) {
                    continue;
                }
                // Never touch another device owner's resources: skip orphans whose exact owner
                // thread (and generation) is not this thread. The rightful owner drains them.
                if (!owned(slot.impl)) {
                    continue;
                }
                if (!retire(slot.impl)) {
                    continue;
                }
                void* const impl = slot.impl;
                slot = Slot{};
                freed[freedCount++] = impl;
            }
        }
        for (std::size_t index = 0; index < freedCount; ++index) {
            destroy(freed[index]);
            retired_.fetch_add(1, std::memory_order_relaxed);
        }
    }

  private:
    struct Slot final {
        void* impl = nullptr;
        bool orphaned = false;
    };

    mutable std::mutex mutex_;
    std::array<Slot, Capacity> slots_{};
    std::atomic<std::uint64_t> refusals_{0};
    std::atomic<std::uint64_t> retired_{0};
};

// Returns the process-wide store for one family. The store is constructed once, in place, in
// function-local static storage and is intentionally NEVER destroyed:
//
//   * Allocation-free: placement new into the static byte buffer constructs the mutex/atomics/array
//     with no heap allocation, so first use cannot throw std::bad_alloc and is safe from a noexcept
//     caller.
//   * No static destructor: non-destruction removes any process-exit ordering concern, and even if a
//     destructor ran it would only destroy a mutex, atomics, and raw void* slots. The store never
//     owns or deletes a native Impl; orphaned Impls are freed only by the explicit owner-thread
//     drain, on the owner thread.
template <std::size_t Capacity>
[[nodiscard]] VoidResidentSlotStore<Capacity>& immortalResidentSlotStore() noexcept {
    alignas(VoidResidentSlotStore<Capacity>) static std::byte storage[sizeof(
        VoidResidentSlotStore<Capacity>)];
    static VoidResidentSlotStore<Capacity>* const store =
        ::new (static_cast<void*>(storage)) VoidResidentSlotStore<Capacity>();
    return *store;
}

} // namespace bloom::render::primitive_detail

#endif // BLOOM_RENDER_VULKAN_GPU_BOUNDED_RETIREMENT_HPP