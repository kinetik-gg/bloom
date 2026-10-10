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

template <std::size_t Capacity> class VoidResidentSlotStore final {
  public:
    static constexpr std::size_t capacity = Capacity;

    VoidResidentSlotStore() = default;
    VoidResidentSlotStore(const VoidResidentSlotStore&) = delete;
    VoidResidentSlotStore& operator=(const VoidResidentSlotStore&) = delete;

    // Acquires a free slot for `impl` before any native allocation. Returns false when the bounded
    // pool is full; the caller refuses cleanly without allocating.
    [[nodiscard]] bool acquire(void* const impl, std::size_t& index,
                               std::size_t admissionLimit = Capacity) noexcept {
        std::lock_guard lock(mutex_);
        if (admissionLimit < Capacity) {
            std::size_t count = 0;
            for (const auto& slot : slots_) {
                count += slot.impl != nullptr ? 1U : 0U;
            }
            if (count >= admissionLimit) {
                refusals_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        for (std::size_t candidate = 0; candidate < Capacity; ++candidate) {
            if (slots_[candidate].impl == nullptr) {
                slots_[candidate] = Slot{.impl = impl};
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
            if (slot.orphaned) {
                pending_.fetch_sub(1, std::memory_order_release);
            }
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
            if (!slot.orphaned) {
                slot.orphaned = true;
                pending_.fetch_add(1, std::memory_order_release);
            }
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
    // so an Impl destructor never re-enters the store. Ownership is only moved out once `retire`
    // has proven the slot safe to free, so an unproven submission is retained intact.
    // Allocation-free and noexcept: the staging array is fixed-capacity.
    template <typename OwnedFn, typename RetireFn, typename DestroyFn>
    void drainOrphans(OwnedFn&& owned, RetireFn&& retire, DestroyFn&& destroy) noexcept {
        if (pending_.load(std::memory_order_acquire) == 0) {
            return;
        }
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
                pending_.fetch_sub(1, std::memory_order_release);
                slot = Slot{};
                freed[freedCount++] = impl;
            }
        }
        for (std::size_t index = 0; index < freedCount; ++index) {
            destroy(freed[index]);
            retired_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Images retain admission and byte metadata until their destructor releases the slot AFTER
    // native destruction. A draining marker prevents a nested/concurrent drain claiming it twice.
    template <typename OwnedFn, typename RetireFn, typename DestroyFn>
    void drainRetainedOrphans(OwnedFn&& owned, RetireFn&& retire, DestroyFn&& destroy) noexcept {
        if (pending_.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::array<void*, Capacity> draining{};
        std::size_t count = 0;
        {
            std::lock_guard lock(mutex_);
            for (auto& slot : slots_) {
                if (slot.impl != nullptr && slot.orphaned && !slot.draining && owned(slot.impl) &&
                    retire(slot.impl)) {
                    slot.draining = true;
                    draining[count++] = slot.impl;
                }
            }
        }
        for (std::size_t index = 0; index < count; ++index) {
            destroy(draining[index]);
            retired_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Claim only orphaned ownership under the short handoff lock. Native polling and destruction
    // run outside it; the exact slot stays occupied/pending until its destructor calls release.
    template <typename OwnedFn, typename RetireFn, typename DestroyFn>
    void drainRetainedOrphansOutsideLock(OwnedFn&& owned, RetireFn&& retire,
                                         DestroyFn&& destroy) noexcept {
        if (pending_.load(std::memory_order_acquire) == 0)
            return;
        for (std::size_t index = 0; index < Capacity; ++index) {
            void* candidate = nullptr;
            {
                std::lock_guard lock(mutex_);
                auto& slot = slots_[index];
                if (slot.impl && slot.orphaned && !slot.draining && !slot.ownerClosed &&
                    owned(slot.impl)) {
                    slot.draining = true;
                    candidate = slot.impl;
                }
            }
            if (!candidate)
                continue;
            if (!retire(candidate)) {
                std::lock_guard lock(mutex_);
                if (slots_[index].impl == candidate)
                    slots_[index].draining = false;
                continue;
            }
            destroy(candidate);
            retired_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Image families attach immutable owner identity and cached native allocation bytes. Metadata
    // is updated and inspected under the same lock; diagnostics never inspect a live Vulkan Impl.
    void setImageMetadata(void* impl, std::size_t index, const void* owner,
                          std::uint64_t bytes) noexcept {
        std::lock_guard lock(mutex_);
        if (index < Capacity && slots_[index].impl == impl) {
            slots_[index].owner = owner;
            slots_[index].bytes = bytes;
        }
    }
    void closeImageOwner(const void* owner) noexcept {
        std::lock_guard lock(mutex_);
        for (auto& slot : slots_) {
            if (slot.impl != nullptr && slot.owner == owner) {
                slot.ownerClosed = true;
            }
        }
    }
    template <typename Visitor> void inspectImageMetadata(Visitor&& visitor) const noexcept {
        std::lock_guard lock(mutex_);
        for (const auto& slot : slots_) {
            if (slot.impl != nullptr) {
                visitor(slot.owner, slot.bytes, slot.orphaned, slot.ownerClosed);
            }
        }
    }

  private:
    struct Slot final {
        void* impl = nullptr;
        bool orphaned = false;
        const void* owner = nullptr;
        std::uint64_t bytes = 0;
        bool ownerClosed = false;
        bool draining = false;
    };

    mutable std::mutex mutex_;
    std::array<Slot, Capacity> slots_{};
    std::atomic<std::size_t> pending_{0};
    std::atomic<std::uint64_t> refusals_{0};
    std::atomic<std::uint64_t> retired_{0};
};

// Returns the process-wide store for one family. The store is constructed once, in place, in
// function-local static storage and is intentionally NEVER destroyed:
//
//   * Allocation-free: placement new into the static byte buffer constructs the mutex/atomics/array
//     with no heap allocation, so first use cannot throw std::bad_alloc and is safe from a noexcept
//     caller.
//   * No static destructor: non-destruction removes any process-exit ordering concern, and even if
//   a
//     destructor ran it would only destroy a mutex, atomics, and raw void* slots. The store never
//     owns or deletes a native Impl; orphaned Impls are freed only by the explicit owner-thread
//     drain, on the owner thread.
//
// `FamilyTag` makes each family's pool a distinct function-local static even when two families
// share a capacity. That is mandatory, not optional: a drain callback casts a slot's opaque Impl
// pointer back to its own family's private Impl type, so two families with different Impl layouts
// sharing one store would let one family's drain reinterpret the other's Impl. There is
// deliberately no default tag, so every caller must name its family explicitly and an omission is a
// compile error. Solid and Upload each pass their own private tag for exactly this reason.
template <std::size_t Capacity, typename FamilyTag>
[[nodiscard]] VoidResidentSlotStore<Capacity>& immortalResidentSlotStore() noexcept {
    alignas(VoidResidentSlotStore<Capacity>) static std::byte
        storage[sizeof(VoidResidentSlotStore<Capacity>)];
    static VoidResidentSlotStore<Capacity>* const store =
        ::new (static_cast<void*>(storage)) VoidResidentSlotStore<Capacity>();
    return *store;
}

} // namespace bloom::render::primitive_detail

#endif // BLOOM_RENDER_VULKAN_GPU_BOUNDED_RETIREMENT_HPP